// window.hpp — Window.contains, a Tier 1 time-and-grid primitive
// (`docs/design/primitives.md:tier1-row:` `anchor − width < t ≤ anchor` —
// the inequality `Window.contains` is the single home of). Deliberately
// dependency-free: no `#include "duckdb.hpp"`, compiles standalone with a
// bare `g++`/`clang++ -std=c++17` — the pattern `grid.hpp` and `kahan.hpp`
// established for Article V.1's TU-per-primitive rule.
//
// Left-open at the anchor minus width, right-closed at the anchor: every
// `RAW_WINDOW` fold reads samples in timestamp order and is "deterministic
// by construction" only because which samples fall inside a window is
// itself unambiguous
// (`docs/design/architecture.md:state-classes:` `deterministic by construction`).
// `docs/testing/primitives.md`'s own reason this primitive exists at all:
// "this primitive is the reason the left-open rule exists in one place"
// (`docs/testing/primitives.md:window-contains-row:` `this primitive is the reason the left-open rule exists in one
// place`). No other file in this kernel spells this inequality itself; every caller goes through `contains` below.
#pragma once

#include <cstdint>

namespace chronoduck {

// A half-open time window `(anchor - width, anchor]`. `width` is the
// caller's precondition to be `>= 0` (a negative width has no defined
// meaning here and isn't validated, the same "caller's precondition, not
// this layer's job" posture `quantile_linear.hpp` documents for its own
// `phi` argument).
struct Window {
	int64_t anchor;
	int64_t width;

	// `anchor − width < t ≤ anchor`, evaluated in `__int128_t` so a window
	// wide enough to underflow `int64_t` at the lower bound (`anchor` near
	// `INT64_MIN`, `width` large) still compares correctly instead of
	// wrapping.
	bool contains(int64_t t) const {
		__int128_t lower = static_cast<__int128_t>(anchor) - static_cast<__int128_t>(width);
		__int128_t tt = static_cast<__int128_t>(t);
		__int128_t aa = static_cast<__int128_t>(anchor);
		return lower < tt && tt <= aa;
	}
};

} // namespace chronoduck
