// grid.hpp — Grid{start,end,step}, a Tier 1 time-and-grid primitive
// (`docs/design/primitives.md:tier1-row:` `Grid{start,end,step}` with `count
// / at / index_of`). Deliberately dependency-free: no `#include
// "duckdb.hpp"`, compiles standalone with a bare `g++`/`clang++ -std=c++17`
// — the pattern `kahan.hpp` and `comparator.hpp` established for Article
// V.1's TU-per-primitive rule.
//
// Time is represented as a raw `int64_t` tick count (microseconds when a
// caller feeds it DuckDB `TIMESTAMP.value`, per
// `docs/design/architecture.md:time-native:` `a superset of the millisecond references`)
// rather than as `duckdb::timestamp_t`, so this header has no DuckDB
// dependency at all; the caller narrows/widens at the boundary.
//
// `index_of`'s floor-division follows `src/chronoduck_extension.cpp`'s own
// `ts_grid_index` convention exactly (widen to `__int128_t` before
// subtracting/dividing so a negative offset floors correctly and a t/start
// pair further apart than `int64_t`'s range can't silently overflow) — that
// scalar function and this primitive are deliberately separate translation
// units (see the issue this header ships with), so the two-line widened
// floor-div is duplicated here rather than shared, the same way
// `comparator.hpp` once carried its own copy of the stale-marker bit pattern
// before `stale.hpp` existed to share it.
#pragma once

#include <cstdint>
#include <stdexcept>

namespace chronoduck {

namespace detail {

// `src/chronoduck_extension.cpp:FloorDiv:` `__int128_t quotient` — genuine
// floor division, not C++'s truncate-toward-zero `/`, widened so the
// subtraction that produces `numerator` elsewhere can't itself have
// overflowed `int64_t` first.
inline __int128_t GridFloorDiv(__int128_t numerator, int64_t denominator) {
	__int128_t quotient = numerator / denominator;
	__int128_t remainder = numerator % denominator;
	if (remainder != 0 && ((remainder < 0) != (denominator < 0))) {
		quotient--;
	}
	return quotient;
}

} // namespace detail

// A regular grid of `count()` points at `start, start+step, ..., end`.
// Construction validates the shape (`docs/testing/primitives.md:grid-row:`
// `step not dividing → construction error; end < start → error`) so every
// live `Grid` is well-formed; `at`/`index_of` are then pure arithmetic with
// no further checking, deliberately unclamped for `i`/`t` outside
// `[0, count)` / `[start, end]` — the same "unclamped is correct, not an
// error" posture `ts_grid_index` documents for itself
// (`src/chronoduck_extension.cpp:TsGridIndexScalarFun:` `Deliberately unclamped`),
// since the invariant below is stated for "random t", not only in-range t.
struct Grid {
	int64_t start;
	int64_t end;
	int64_t step;

	Grid(int64_t start_, int64_t end_, int64_t step_) : start(start_), end(end_), step(step_) {
		if (step_ <= 0) {
			throw std::invalid_argument("Grid: step must be positive");
		}
		if (end_ < start_) {
			throw std::invalid_argument("Grid: end must be >= start");
		}
		if ((end_ - start_) % step_ != 0) {
			throw std::invalid_argument("Grid: step must evenly divide end - start");
		}
	}

	// The number of grid points, inclusive of both `start` and `end` —
	// `docs/testing/primitives.md:grid-row:` `count == (end−start)/step + 1`.
	int64_t count() const {
		return (end - start) / step + 1;
	}

	// The timestamp of grid point `i`. Not bounds-checked against
	// `[0, count)`: `index_of(t) + 1` is a valid argument even when `t` is
	// the grid's last point, which is exactly what the invariant below
	// exercises.
	int64_t at(int64_t i) const {
		return start + i * step;
	}

	// The index of the grid point at or before `t` — `floor((t - start) /
	// step)` — widened the same way `ts_grid_index` widens, so a `t` before
	// `start` (negative offset) floors correctly instead of truncating
	// toward zero.
	int64_t index_of(int64_t t) const {
		__int128_t offset = static_cast<__int128_t>(t) - static_cast<__int128_t>(start);
		__int128_t index = detail::GridFloorDiv(offset, step);
		return static_cast<int64_t>(index);
	}
};

} // namespace chronoduck
