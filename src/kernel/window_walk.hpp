// window_walk.hpp — window_walk, a Tier 3 window-iteration primitive
// (`docs/design/primitives.md:tier3-row:` `window_walk` (two monotone
// pointers, O(n + steps))). Deliberately dependency-free: no `#include
// "duckdb.hpp"`, compiles standalone with a bare `g++`/`clang++ -std=c++17`
// — the pattern `grid.hpp`, `window.hpp`, `scan_bounds.hpp` and
// `sample_buffer.hpp` established for Article V.1's TU-per-primitive rule.
//
// Walks every point of a `Grid` against one sorted, deduplicated
// `SampleBuffer` (`sort_dedup`'s own postcondition), producing, for grid
// point `i`, the half-open sample-index range `[lo, hi)` such that
// `Window{grid.at(i), width}.contains(data[j].t)` holds for exactly the `j`
// in that range. `docs/design/architecture.md:where-it-plugs-in:` `streams
// each partition through a two-pointer walk that holds one window` is this
// primitive's own reason to exist: the operator's per-partition walk over a
// sorted, buffer-managed block is this same two-pointer technique, applied
// grid point by grid point.
//
// The two pointers advance forward only, across the *whole* grid walk, not
// once per grid point: `grid.at(i)` is non-decreasing in `i`, so both the
// window's lower edge (`grid.at(i) - width`) and upper edge (`grid.at(i)`)
// are non-decreasing across the walk, and `data` is sorted ascending by
// `t` — which is exactly what makes a pointer that never resets still find
// the right answer at every step. Each pointer therefore visits at most `n`
// samples in total across the entire walk (never `n` per grid point), for
// O(n + grid.count()) total work — `docs/testing/primitives.md:window-walk-row:`
// `total work is O(n + steps)`.
//
// `hi` needs only a plain `t <= anchor` comparison (no width term at all).
// `lo` needs the window's *lower* edge, `anchor - width`, but per
// `window.hpp`'s own header comment — "No other file in this kernel spells
// this inequality itself; every caller goes through `contains` below." —
// this file never recomputes that subtraction itself. Instead `lo` advances past
// every sample that is not yet admitted by `Window::contains` *and* has not
// already passed `anchor` — the second clause is what tells the empty-window
// case (no sample ever satisfies `contains`) to stop `lo` exactly where `hi`
// stops, rather than running `lo` past the whole buffer. Since `data` is
// sorted and `contains` is true on exactly one contiguous run for a fixed
// `anchor`/`width`, `!contains(t) && t <= anchor` holds precisely for
// `t <= anchor - width` — the lower-edge condition, reached without ever
// spelling `anchor - width` in this file.
#pragma once

#include "grid.hpp"
#include "sample_buffer.hpp"
#include "window.hpp"

#include <cstddef>
#include <vector>

namespace chronoduck {

// The half-open range `[lo, hi)` of indices into the buffer passed to
// `window_walk`, admitted by one grid point's window.
struct WindowRange {
	std::size_t lo;
	std::size_t hi;
};

// `data[0..n)` must be sorted ascending by `t` with no duplicate
// timestamps — `SampleBuffer::sort_dedup`'s own postcondition — unchecked
// here, the same "caller's precondition, not this layer's job" posture
// `grid.hpp`/`window.hpp`/`sample_buffer.hpp` document for themselves.
// `width` is `Window`'s own precondition to be `>= 0`.
inline std::vector<WindowRange> window_walk(const Sample *data, std::size_t n, const Grid &grid, int64_t width) {
	std::vector<WindowRange> ranges;
	ranges.reserve(static_cast<std::size_t>(grid.count()));

	std::size_t lo = 0, hi = 0;
	for (int64_t i = 0; i < grid.count(); i++) {
		int64_t anchor = grid.at(i);
		Window w {anchor, width};

		while (lo < n && !w.contains(data[lo].t) && data[lo].t <= anchor) {
			lo++;
		}
		while (hi < n && data[hi].t <= anchor) {
			hi++;
		}

		ranges.push_back({lo, hi});
	}

	return ranges;
}

} // namespace chronoduck
