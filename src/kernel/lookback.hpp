// lookback.hpp — lookback_bound and the carry rule, a Tier 1 time-and-grid
// primitive (`docs/design/primitives.md:tier1-row:` `lookback_bound` and
// the `carry` rule). Deliberately dependency-free: no `#include
// "duckdb.hpp"`, compiles standalone with a bare `g++`/`clang++ -std=c++17`
// — the pattern `grid.hpp` and `window.hpp` established for Article V.1's
// TU-per-primitive rule.
//
// This is the LOOKBACK edge mode's own machinery
// (`docs/design/architecture.md:edge-modes:` `carry within lookback; a stale marker ends the carry`):
// a single sample carries
// forward to `anchor` if it falls within `lookback` of `anchor` — closed on
// *both* ends, deliberately unlike `Window.contains`'s left-open interval,
// per `docs/testing/primitives.md`'s own unit contract for this primitive:
// "Sample exactly at the bound (carried), one µs older (not)" — the bound
// itself carries, so the lower edge is `<=`, not `<`.
#pragma once

#include "stale.hpp"

#include <cstdint>

namespace chronoduck {

// The earliest timestamp (inclusive) that may still carry to `anchor` under
// a `lookback`-wide carry window.
inline int64_t lookback_bound(int64_t anchor, int64_t lookback) {
	__int128_t bound = static_cast<__int128_t>(anchor) - static_cast<__int128_t>(lookback);
	return static_cast<int64_t>(bound);
}

// The carry rule: `sample_t` carries forward to `anchor` iff it lies in the
// closed interval `[lookback_bound(anchor, lookback), anchor]` *and*
// `sample_v` is not the stale marker — a stale marker ends the carry
// (`docs/design/architecture.md:edge-modes:` `a stale marker ends the carry`)
// even when it would otherwise be the freshest sample in range.
inline bool carry(int64_t anchor, int64_t lookback, int64_t sample_t, double sample_v) {
	if (is_stale(sample_v)) {
		return false;
	}
	__int128_t bound = static_cast<__int128_t>(anchor) - static_cast<__int128_t>(lookback);
	__int128_t st = static_cast<__int128_t>(sample_t);
	__int128_t aa = static_cast<__int128_t>(anchor);
	return bound <= st && st <= aa;
}

} // namespace chronoduck
