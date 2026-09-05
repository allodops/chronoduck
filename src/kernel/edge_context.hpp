// edge_context.hpp — edge_context, a Tier 3 window-iteration primitive
// (`docs/design/primitives.md:tier3-row:` `edge_context`): the last sample
// at or before `t − w` — a sample Window.contains's own left-open bound
// excludes from the primary window — and the first sample after `t`.
// Deliberately dependency-free: no `#include "duckdb.hpp"`, compiles
// standalone with a bare `g++`/`clang++ -std=c++17` — the pattern
// `window_walk.hpp` and `sample_buffer.hpp` established for Article V.1's
// TU-per-primitive rule.
//
// ANCHOR reads the last sample at or before the window (`t − w`) instead of
// the primary window at all (`docs/design/architecture.md:edge-modes:`
// `read the last sample before the window`); SMOOTH reads that same
// "before" edge plus the first sample after `t` to interpolate both edges
// (`docs/design/architecture.md:edge-modes:` `interpolate both edges`).
// This primitive is exactly those two lookups, independent of
// `window_walk`'s own INSIDE-window pointers: an ANCHOR fold never touches
// `window_walk` at all, so `edge_context` cannot be built as a mere
// accessor into `window_walk`'s state — it does its own pointer work over
// the buffer's own storage.
//
// `before` and `after` are single-sided comparisons, not the windowed
// double inequality `Window.contains` owns: `before`'s threshold
// `anchor - width`, widened to `__int128_t` before subtracting, is the same
// shape `lookback.hpp`'s own `lookback_bound` widens its `anchor - lookback`
// subtraction with — one more standalone copy of the overflow-safe-subtraction
// pattern `grid.hpp`'s own header comment already accepts duplicating across
// dependency-free TUs rather than adding a cross-primitive include for two
// lines of arithmetic.
#pragma once

#include "sample_buffer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace chronoduck {

// `before`/`after` are valid only when the matching `has_*` flag is set —
// there may be no sample on one side or the other (an empty buffer, or `t`
// at the very start/end of the data).
struct EdgeContext {
	bool has_before;
	Sample before;
	bool has_after;
	Sample after;
};

// `data[0..n)` must be sorted ascending by `t` with no duplicate
// timestamps — `SampleBuffer::sort_dedup`'s own postcondition — unchecked
// here, the same "caller's precondition, not this layer's job" posture
// `window_walk.hpp` documents for itself. `width` is `Window`'s own
// precondition to be `>= 0`.
inline EdgeContext edge_context(const Sample *data, std::size_t n, int64_t anchor, int64_t width) {
	const Sample *begin = data;
	const Sample *end = data + n;

	// `before`: the last sample with `t <= anchor - width`, widened the
	// same way `lookback_bound` widens so a `width` large relative to
	// `anchor` can't overflow before comparing. `std::upper_bound` finds
	// the first sample *past* that threshold; the sample just before it —
	// if any — is the last one at or before it, by sortedness.
	__int128_t threshold = static_cast<__int128_t>(anchor) - static_cast<__int128_t>(width);
	const Sample *before_it = std::upper_bound(
	    begin, end, threshold, [](__int128_t th, const Sample &s) { return th < static_cast<__int128_t>(s.t); });
	bool has_before = before_it != begin;
	Sample before = has_before ? *(before_it - 1) : Sample {0, 0.0};

	// `after`: the first sample with `t > anchor` — a plain comparison,
	// no width term and no widening needed.
	const Sample *after_it = std::upper_bound(begin, end, anchor, [](int64_t a, const Sample &s) { return a < s.t; });
	bool has_after = after_it != end;
	Sample after = has_after ? *after_it : Sample {0, 0.0};

	return EdgeContext {has_before, before, has_after, after};
}

// `SampleSpan` convenience overload — `window_walk.hpp`'s callers hold a
// `SampleBuffer`/`SampleSpan`, not a raw pointer and count.
inline EdgeContext edge_context(SampleSpan span, int64_t anchor, int64_t width) {
	return edge_context(span.begin, span.size(), anchor, width);
}

} // namespace chronoduck
