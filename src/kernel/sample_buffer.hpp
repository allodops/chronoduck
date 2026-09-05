// sample_buffer.hpp — Sample{t,v}, the arena-backed SampleBuffer
// (append / merge / sort_dedup / slice) and dedup_policy, three of the five
// primitives docs/design/primitives.md's Tier 2 row names
// (`docs/design/primitives.md:tier2-row:` `arena-backed`). `HistSample`,
// `LastK<n>` and `SlicePartials` are that row's other primitives but are
// out of this issue's stated scope (the issue this header ships with lists
// them under "Out of scope"); they land with the tiers that actually need
// them — histograms, LAST_K and SLICE — none of which M1's `ts_rate` touches.
// Deliberately dependency-free: no `#include "duckdb.hpp"`, compiles
// standalone with a bare `g++`/`clang++ -std=c++17` — the pattern
// `kahan.hpp`, `grid.hpp` and `window.hpp` established for Article V.1's
// TU-per-primitive rule.
//
// dedup_policy is the declared duplicate-timestamp tie-break
// (`docs/decisions/0004-totalorder-tie-break.md`, restated by
// `docs/design/architecture.md`'s own numeric-contracts paragraph): within
// one series, two samples at the same timestamp resolve by keeping the
// value that is greatest under IEEE 754 totalOrder
// (`docs/design/architecture.md:duplicate-timestamps:` `keep the greatest
// under totalOrder, except that any NaN loses to any non-NaN and the stale
// marker loses to an ordinary NaN`) — `−0 < +0`, NaN payloads ordered by
// their raw bit pattern, except that any NaN loses to any non-NaN and the
// stale marker (itself a specific NaN payload, `stale.hpp`) loses to an
// ordinary NaN. `SampleBuffer::sort_dedup` is this rule's only caller in
// the kernel; the architecture doc's own closing sentence for the rule is
// "The operator deduplicates within a series run itself and never relies
// on the sort to do it."
#pragma once

#include "stale.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

namespace chronoduck {

// One (timestamp, value) reading. `t` is a raw tick count, the same
// DuckDB-independent representation `grid.hpp`/`window.hpp` use.
struct Sample {
	int64_t t;
	double v;
};

namespace detail {

inline uint64_t SampleBufferBits(double v) {
	uint64_t bits;
	std::memcpy(&bits, &v, sizeof(bits));
	return bits;
}

constexpr uint64_t kSignBit = 0x8000000000000000ULL;

// IEEE 754 totalOrder expressed as a monotonic unsigned key: a negative
// value's bit pattern is fully inverted, a non-negative one has its sign
// bit set, mapping the entire raw bit space — finite values, ±Inf, every
// NaN payload, signed zero included — onto one ascending `uint64_t` order.
// `-0.0`'s bit pattern (`kSignBit`, every other bit zero) keys to
// `~kSignBit` (`kSignBit - 1`); `+0.0`'s bit pattern (all-zero) keys to
// `kSignBit` — one greater — so `-0 < +0` falls out of the transform
// itself rather than needing a special case
// (`docs/design/architecture.md:duplicate-timestamps:` `−0 < +0`).
inline uint64_t TotalOrderKey(double v) {
	uint64_t bits = SampleBufferBits(v);
	return (bits & kSignBit) ? ~bits : (bits | kSignBit);
}

// The declared tie-break rank: `tier` places the stale marker below every
// ordinary NaN, and every ordinary NaN below every non-NaN value — the
// whole reason `dedup_policy` exists instead of a bare `TotalOrderKey`
// comparison, which would rank NaNs at the *extremes* instead. `key` orders
// same-tier values by `TotalOrderKey`. `TotalOrderKey` is injective over
// every `double` bit pattern, so no two distinct samples ever produce equal
// `(tier, key)` pairs: `RankLess` below is a strict total order, which is
// what makes a duplicate-timestamp group's winner independent of the order
// its candidates happen to be visited in.
struct DedupRank {
	int tier;
	uint64_t key;
};

inline DedupRank RankOf(double v) {
	if (is_stale(v)) {
		return {0, TotalOrderKey(v)};
	}
	if (std::isnan(v)) {
		return {1, TotalOrderKey(v)};
	}
	return {2, TotalOrderKey(v)};
}

inline bool RankLess(const DedupRank &a, const DedupRank &b) {
	if (a.tier != b.tier) {
		return a.tier < b.tier;
	}
	return a.key < b.key;
}

} // namespace detail

// dedup_policy — true iff `challenger` must replace `incumbent` as the kept
// value for a duplicate timestamp, per the rule this header's own comment
// above restates from ADR 0004. Scanning a timestamp group in *any* order,
// keeping whichever candidate this returns true against a running best,
// always converges on the same winner (`detail::RankLess` is a strict total
// order — see `detail::DedupRank`'s own comment) — which is what makes
// `SampleBuffer::sort_dedup` correct regardless of how its internal sort
// happens to order same-timestamp samples.
inline bool dedup_policy(double challenger, double incumbent) {
	return detail::RankLess(detail::RankOf(incumbent), detail::RankOf(challenger));
}

namespace detail {

// The byte-tracked arena `SampleBuffer` bump-allocates into. Growth
// allocates a new page and never copies samples already appended into a
// prior page — `docs/testing/benchmarks.md`'s own allocation-count law for
// this primitive (`docs/testing/benchmarks.md:allocations-law:` `one per
// series plus one per arena page`) is what this is for: an arena, not a
// `std::vector` wearing a different name. `byte_count` is the allocator's
// byte counter this issue's acceptance criteria pin at zero after destroy;
// every page's bytes are subtracted from it in `release_all`, called from
// both the destructor and `compact_to`, so a forgotten page free — the
// SampleBuffer testing row's own "a missed page free" must-die mutant
// (`docs/testing/primitives.md:samplebuffer-row:` `a missed page free`) —
// is exactly what would leave this counter nonzero.
class SampleArena {
public:
	SampleArena() = default;
	~SampleArena() {
		release_all();
	}

	SampleArena(const SampleArena &) = delete;
	SampleArena &operator=(const SampleArena &) = delete;

	SampleArena(SampleArena &&other) noexcept {
		*this = std::move(other);
	}
	SampleArena &operator=(SampleArena &&other) noexcept {
		if (this != &other) {
			release_all();
			pages_ = std::move(other.pages_);
			bytes_allocated_ = other.bytes_allocated_;
			other.pages_.clear();
			other.bytes_allocated_ = 0;
		}
		return *this;
	}

	// Bump-allocates room for exactly one more sample, growing by a new
	// page (doubling capacity) when the current page is full. The returned
	// pointer stays valid for this arena's lifetime: earlier pages are
	// never moved or reallocated, which is what this row's own unit
	// contract for append means to test
	// (`docs/testing/primitives.md:samplebuffer-row:` `growth across arena pages`).
	Sample *push() {
		if (pages_.empty() || pages_.back().used == pages_.back().capacity) {
			add_page(pages_.empty() ? kInitialPageCapacity : pages_.back().capacity * 2);
		}
		Page &page = pages_.back();
		return &page.data[page.used++];
	}

	std::size_t byte_count() const {
		return bytes_allocated_;
	}
	std::size_t page_count() const {
		return pages_.size();
	}
	std::size_t size() const {
		std::size_t n = 0;
		for (const Page &page : pages_) {
			n += page.used;
		}
		return n;
	}

	// Replaces every page with one freshly-allocated page holding exactly
	// `samples` (in order), releasing the pages it replaces first —
	// `sort_dedup`'s compaction step, and the one place a leaked page would
	// surface as a nonzero `byte_count` once the caller destroys the
	// buffer.
	void compact_to(const std::vector<Sample> &samples) {
		release_all();
		if (samples.empty()) {
			return;
		}
		add_page(samples.size());
		Page &page = pages_.back();
		std::memcpy(page.data, samples.data(), samples.size() * sizeof(Sample));
		page.used = samples.size();
	}

	// Visits every sample currently held, across every page, in page/append
	// order (not necessarily sorted) — `merge`'s and `sort_dedup`'s own
	// walk over raw storage.
	template <typename Fn>
	void for_each(Fn &&fn) const {
		for (const Page &page : pages_) {
			for (std::size_t i = 0; i < page.used; i++) {
				fn(page.data[i]);
			}
		}
	}

	// The first page's own contiguous storage — meaningful only when this
	// arena holds at most one page, which `compact_to` guarantees.
	// `SampleBuffer::slice`'s precondition (sort_dedup since the last
	// append/merge) is what keeps this to "at most one page" in practice;
	// unchecked here, the same "caller's precondition, not this layer's
	// job" posture `grid.hpp`/`window.hpp` document for themselves.
	const Sample *front_page_data() const {
		return pages_.empty() ? nullptr : pages_.front().data;
	}
	std::size_t front_page_size() const {
		return pages_.empty() ? 0 : pages_.front().used;
	}

private:
	static constexpr std::size_t kInitialPageCapacity = 4;

	struct Page {
		Sample *data;
		std::size_t capacity;
		std::size_t used;
	};

	std::vector<Page> pages_;
	std::size_t bytes_allocated_ = 0;

	void add_page(std::size_t capacity) {
		Sample *data = static_cast<Sample *>(::operator new(capacity * sizeof(Sample)));
		bytes_allocated_ += capacity * sizeof(Sample);
		pages_.push_back(Page {data, capacity, 0});
	}

	void release_all() {
		for (Page &page : pages_) {
			bytes_allocated_ -= page.capacity * sizeof(Sample);
			::operator delete(page.data);
		}
		pages_.clear();
	}
};

} // namespace detail

// A read-only view into a `SampleBuffer`'s own contiguous storage — the
// result of `slice`. Never owns memory; valid only as long as the buffer it
// came from stays alive and unmutated.
struct SampleSpan {
	const Sample *begin;
	const Sample *end;

	std::size_t size() const {
		return static_cast<std::size_t>(end - begin);
	}
};

// Arena-backed sample storage for one series
// (`docs/design/primitives.md:tier2-row:` `dedup_policy`). `append`/`merge`
// accumulate raw samples — possibly with duplicate or out-of-order
// timestamps — across however many arena pages that takes; `sort_dedup` is
// the one place that imposes order, resolving duplicates by `dedup_policy`
// and compacting the result into a single page so `slice` can binary-search
// a plain contiguous, sorted array. Calling `slice` before `sort_dedup` (or
// after a further `append`/`merge`) is a caller error this layer does not
// check for — again, `grid.hpp`/`window.hpp`'s own "caller's precondition"
// posture.
class SampleBuffer {
public:
	Sample *append(int64_t t, double v) {
		Sample *slot = arena_.push();
		slot->t = t;
		slot->v = v;
		return slot;
	}

	Sample *append(Sample s) {
		return append(s.t, s.v);
	}

	// Concatenates `other`'s raw samples onto this buffer — no sorting, no
	// deduplication: this row's own invariant is that merging and then
	// sort_dedup-ing equals sort_dedup on the plain concatenation, bit-exact
	// (`docs/testing/primitives.md:samplebuffer-row:` `of the concatenation, bit-exact`).
	// `other` is left unmodified.
	void merge(const SampleBuffer &other) {
		other.arena_.for_each([this](const Sample &s) { append(s); });
	}

	std::size_t size() const {
		return arena_.size();
	}
	std::size_t page_count() const {
		return arena_.page_count();
	}
	std::size_t byte_count() const {
		return arena_.byte_count();
	}

	// Sorts by timestamp, resolves every duplicate-timestamp group by
	// `dedup_policy`, and compacts the result into a single page. After
	// this call, `size()` is the number of *distinct* timestamps and the
	// buffer's storage is one contiguous, strictly-increasing-by-`t` array
	// — `slice`'s precondition.
	void sort_dedup() {
		std::vector<Sample> all;
		all.reserve(arena_.size());
		arena_.for_each([&all](const Sample &s) { all.push_back(s); });

		std::stable_sort(all.begin(), all.end(), [](const Sample &a, const Sample &b) { return a.t < b.t; });

		std::vector<Sample> deduped;
		deduped.reserve(all.size());
		for (const Sample &s : all) {
			if (!deduped.empty() && deduped.back().t == s.t) {
				if (dedup_policy(s.v, deduped.back().v)) {
					deduped.back().v = s.v;
				}
			} else {
				deduped.push_back(s);
			}
		}

		arena_.compact_to(deduped);
	}

	// The maximal contiguous run of this buffer's own storage with
	// `lo <= t && t <= hi` — a plain closed interval; a caller needing
	// `Window.contains`'s left-open convention applies it on top of this
	// (this primitive is generic, `window.hpp`'s inequality is not
	// duplicated here). Precondition: `sort_dedup` since the last
	// `append`/`merge` — unchecked, per this header's own stated posture
	// above.
	SampleSpan slice(int64_t lo, int64_t hi) const {
		const Sample *data = arena_.front_page_data();
		std::size_t n = arena_.front_page_size();
		const Sample *lo_it = std::lower_bound(data, data + n, lo, [](const Sample &s, int64_t t) { return s.t < t; });
		const Sample *hi_it = std::upper_bound(lo_it, data + n, hi, [](int64_t t, const Sample &s) { return t < s.t; });
		return {lo_it, hi_it};
	}

private:
	detail::SampleArena arena_;
};

} // namespace chronoduck
