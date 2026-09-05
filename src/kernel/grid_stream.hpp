// grid_stream.hpp — GridStream, a Tier 3 window-iteration primitive
// (`docs/design/primitives.md:tier3-row:` `grid_stream` (the operator's
// per-partition walk: same pointers over the partition's sorted stream, one
// window resident, buffers released at each series boundary)`). Deliberately
// dependency-free: no `#include "duckdb.hpp"`, compiles standalone with a
// bare `g++`/`clang++ -std=c++17` — the pattern `window_walk.hpp` and
// `counter_fold.hpp` established for Article V.1's TU-per-primitive rule.
//
// `window_walk` (Tier 3's own sibling primitive) walks a *fully materialized*
// sorted array — its own precondition is `SampleBuffer::sort_dedup`'s
// postcondition, a single contiguous buffer already holding the whole
// partition. The operator (`docs/decisions/0003-operator-as-partition-sort-sink.md`)
// cannot afford that: its memory law is "O(range) in a spillable sort plus
// O(threads × window) resident" — never O(range) resident per series, which
// is exactly what handing `window_walk` a fully materialized per-series
// buffer would cost. `GridStream` is `window_walk`'s same two-pointer
// invariant (`grid.at(i)` and `anchor - width` are both non-decreasing across
// the whole walk, so a pointer that never resets still finds the right
// answer at every step), computed incrementally against a stream of samples
// fed one at a time instead of an array — holding only the samples currently
// inside `(anchor - width, anchor]` or not yet ruled out of a future window,
// dropping everything the walk has permanently passed. That resident set is
// bounded by the number of samples in one window (`docs/testing/memory.md`'s
// own "one window per thread is resident" law), not by the partition's whole
// range.
//
// Dedup ("the operator deduplicates within a series run itself and never
// relies on the sort to do it", `docs/design/architecture.md:duplicate-timestamps:`)
// is folded into the same incremental pipeline: the caller's own sort
// guarantees non-decreasing `t`, but says nothing about which of two samples
// sharing a timestamp comes first, so `GridStream::feed` collapses adjacent
// equal-`t` samples via `dedup_policy` (`sample_buffer.hpp`'s own rule) as
// they arrive, one pending sample of lookahead — the same rule
// `SampleBuffer::sort_dedup` applies in bulk, applied one timestamp group at
// a time instead.
#pragma once

#include "counter_fold.hpp"
#include "grid.hpp"
#include "sample_buffer.hpp"
#include "window.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace chronoduck {

// Streams one series' samples — fed in non-decreasing `t` order, duplicate
// timestamps allowed and resolved internally — against every point of a
// `Grid`, calling `emit(grid_index, data, n)` for each grid point as soon as
// its window is fully determined (a sample past the window's upper edge has
// arrived, or `end()` has been called). `data[0..n)` is the same `[lo, hi)`
// view `window_walk` would have produced from a full array — a contiguous
// span of `CounterSample`s in timestamp order, valid only for the duration
// of the `emit` call.
class GridStream {
public:
	GridStream(Grid grid, int64_t width) : grid_(grid), width_(width) {
	}

	// Feeds one raw sample. `t` must be >= every `t` already fed (the
	// caller's own sortedness precondition — the same "caller's
	// precondition, not this layer's job" posture `window_walk.hpp`/
	// `sample_buffer.hpp` document for themselves). Two samples sharing a
	// timestamp collapse via `dedup_policy` on `.v`; the survivor keeps
	// whichever sample's `has_st`/`st` came with the surviving `.v` — the
	// same pairing `sort_dedup` + `st_by_t`'s join produces today in
	// `src/chronoduck_extension.cpp:ComputeRatePoints:` `payload.st_pairs`.
	template <typename Emit>
	void feed(CounterSample s, Emit &emit) {
		if (has_pending_ && s.t == pending_.t) {
			if (dedup_policy(s.v, pending_.v)) {
				pending_ = s;
			}
			return;
		}
		flush_pending();
		pending_ = s;
		has_pending_ = true;
		drain(/*eos=*/false, emit);
	}

	// Marks the series exhausted: no future sample can ever arrive, so every
	// grid point still waiting on the upper edge is now fully determined.
	// Call exactly once, after the last `feed`. `resume` (below) is what
	// actually drains the remaining grid points — `end` only flushes the one
	// sample still held back by dedup's one-sample lookahead.
	void end() {
		flush_pending();
		ended_ = true;
	}

	// Resumes draining after `feed`/`end` stopped early (only possible if the
	// caller's own `emit` throws or otherwise never returns cleanly — in
	// ordinary use `feed`/`end` already drain everything they can). Also the
	// only way to progress once `end()` has been called, since `end()`
	// itself does not drain. A no-op once `done()`.
	template <typename Emit>
	void resume(Emit &emit) {
		drain(ended_, emit);
	}

	// True once every grid point has been emitted.
	bool done() const {
		return next_index_ >= grid_.count();
	}

	// The buffer's current physical size — samples currently resident,
	// including the amortized-compaction slack `drain`'s erase threshold
	// allows. Exposed for the residency-bound test (`docs/testing/memory.md`'s
	// own "one window per thread is resident" law is this primitive's whole
	// reason to exist); not used by `feed`/`resume` themselves.
	std::size_t resident_size() const {
		return buf_.size();
	}

private:
	void flush_pending() {
		if (!has_pending_) {
			return;
		}
		buf_.push_back(pending_);
		has_pending_ = false;
	}

	// The two-pointer walk itself, `window_walk`'s own per-grid-point body
	// (`lo`/`hi` monotone non-decreasing across the whole walk), applied to
	// however much of `buf_` is available right now. `eos` (end of stream)
	// is what tells `hi` that "ran out of buffered data" means "there is no
	// more data", not "wait for more" — the one behavioural difference from
	// a single offline pass over a full array, where `n` never grows after
	// the walk starts.
	template <typename Emit>
	void drain(bool eos, Emit &emit) {
		while (next_index_ < grid_.count()) {
			int64_t anchor = grid_.at(next_index_);

			while (hi_ < buf_.size() && buf_[hi_].t <= anchor) {
				hi_++;
			}
			if (hi_ == buf_.size() && !eos) {
				return; // hi not yet confirmed stopped; need more samples
			}

			Window w {anchor, width_};
			while (lo_ < hi_ && !w.contains(buf_[lo_].t) && buf_[lo_].t <= anchor) {
				lo_++;
			}

			emit(next_index_, buf_.data() + lo_, hi_ - lo_);
			next_index_++;

			// Drop every sample lo_ has permanently passed: window_walk's own
			// invariant (`lo`/`hi` never decrease across the walk) means no
			// future grid point can ever need index < lo_ again. Amortized
			// O(1) per fed sample — the erase only fires once the dropped
			// prefix is a real fraction of the buffer, not on every point.
			if (lo_ > 0 && lo_ * 2 > buf_.size()) {
				buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(lo_));
				hi_ -= lo_;
				lo_ = 0;
			}
		}
	}

	Grid grid_;
	int64_t width_;

	// The dedup lookahead: the most recently fed sample not yet known to be
	// the final winner for its timestamp (one more sample at the same `t`
	// could still beat it).
	bool has_pending_ = false;
	CounterSample pending_ {};

	// The window's own resident buffer: every fed-and-deduplicated sample
	// from the current `lo_` position onward. Bounded by the number of
	// samples in one window once compaction has run — never by the
	// partition's whole range.
	std::vector<CounterSample> buf_;
	std::size_t lo_ = 0;
	std::size_t hi_ = 0;
	int64_t next_index_ = 0;
	bool ended_ = false;
};

} // namespace chronoduck
