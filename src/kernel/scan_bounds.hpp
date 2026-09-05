// scan_bounds.hpp — scan_bounds(grid, window, lookback, mode), a Tier 1
// time-and-grid primitive (`docs/design/primitives.md:tier1-row:`
// `scan_bounds(grid, window, lookback, mode)`). Deliberately
// dependency-free: no `#include "duckdb.hpp"`, compiles standalone with a
// bare `g++`/`clang++ -std=c++17` — the pattern `grid.hpp`, `window.hpp` and
// `lookback.hpp` established for Article V.1's TU-per-primitive rule.
//
// This is the primitive the optimizer-extension pushdown
// (`docs/design/architecture.md:where-it-plugs-in:` `pushes the scan bound`)
// will eventually call; wiring it into that optimizer extension is
// plan-level pushdown and stays out of this issue's scope (its own "Out of
// scope" line) — this header only computes the bound.
//
// `docs/design/architecture.md`'s own formula for the bound is
// `[start − window − lookback − anchor_extra, end + lookahead]`
// (`docs/design/architecture.md:where-it-plugs-in:` `start − window − lookback − anchor_extra`),
// naming four independent terms; but this primitive's declared signature —
// `docs/design/primitives.md`'s own `scan_bounds(grid, window, lookback,
// mode)` — carries only one caller-supplied "how far beyond the window"
// scalar, `lookback`. Deviation (see the PR description): `anchor_extra` and
// `lookahead` are not separate parameters here, so ANCHOR and SMOOTH both
// reuse the single `lookback` argument as their own extra magnitude — an
// ANCHOR row's "how far back may the anchor sample be" and a SMOOTH row's
// "how far ahead may the right-edge sample be" are the same *kind* of
// quantity LOOKBACK's own
// (`docs/design/architecture.md:edge-modes:` `carry within lookback`) carry
// distance is, so one scalar serves all three. Each edge-mode term is added
// independently of the others (never taking a max or replacing the base
// term), which keeps the result a safe superset even when a caller sets more
// than one bit — `docs/testing/primitives.md:scan-bounds-row:` `Every sample
// that any fold on this grid could read lies inside the bounds` only
// requires supersetting, never tightness.
#pragma once

#include "grid.hpp"
#include "registry_types.hpp"

#include <cstdint>

namespace chronoduck {

struct ScanBounds {
	int64_t lower;
	int64_t upper;
};

// `mode` is the `EdgeMode` bitmask (`src/kernel/registry_types.hpp:EdgeMode:`
// `enum EdgeMode : unsigned {`) a row declares support for.
// `docs/testing/primitives.md:scan-bounds-row:`
// `+lookback for LOOKBACK; −extra for ANCHOR; +lookahead for SMOOTH`:
// INSIDE (or no recognized bit) is the base `[start − window, end]`; LOOKBACK
// and ANCHOR each subtract `lookback` again from the lower bound (per the
// header comment's reuse deviation); SMOOTH adds `lookback` to the upper
// bound. Widened to `__int128_t` throughout so a `window`/`lookback` large
// relative to `grid.start`/`grid.end` can't overflow before narrowing back.
inline ScanBounds scan_bounds(const Grid &grid, int64_t window, int64_t lookback, unsigned mode) {
	__int128_t lower = static_cast<__int128_t>(grid.start) - static_cast<__int128_t>(window);
	if (mode & LOOKBACK) {
		lower -= lookback;
	}
	if (mode & ANCHOR) {
		lower -= lookback;
	}
	__int128_t upper = static_cast<__int128_t>(grid.end);
	if (mode & SMOOTH) {
		upper += lookback;
	}
	return ScanBounds {static_cast<int64_t>(lower), static_cast<int64_t>(upper)};
}

} // namespace chronoduck
