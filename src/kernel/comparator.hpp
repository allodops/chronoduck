// comparator.hpp — the one tolerance derivation `docs/testing/rules.md`
// binds as T3 ("one comparator, one derivation, applied to every value in
// every layer"), plus the L13 headroom pin that guards it from eroding.
// Deliberately dependency-free: no `#include "duckdb.hpp"` and no other
// DuckDB header, so this file compiles standalone with a bare
// `g++`/`clang++ -std=c++17` — the same pattern `registry_types.hpp`
// established for `registry.def` (Article V.1).
//
// `equal_values` below transcribes `docs/testing/comparator.md`'s own shown
// derivation verbatim (constants, branch order and comments included)
// rather than paraphrasing it, per Article V.2 ("`docs/testing/` is
// binding").
#pragma once

#include "stale.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace chronoduck {

// `docs/testing/comparator.md:kReorderFactor:` `≈ 9.09e-13`
constexpr double kUnitRoundoff = 1.0 / (1ull << 53);                              // u = 2^-53
constexpr int kMaxReorderedSamples = 4096;                                        // stated budget, > 1 h at 1 s
constexpr double kReorderFactor = 2 * (kMaxReorderedSamples - 1) * kUnitRoundoff; // ≈ 9.09e-13

// The reference's staleness NaN, bit-for-bit
// (`docs/design/architecture.md:staleness:` `bit-for-bit`). `equal_values`
// below transcribes `docs/testing/comparator.md`'s own shown derivation
// verbatim, `is_stale_nan` name included — this is now a one-line delegate
// to the shared `is_stale`/`stale_marker` primitive `src/kernel/stale.hpp`
// (#28/T1.4) adds, so the bit pattern itself has exactly one definition in
// the tree; only the name stays local, to keep the transcription verbatim.
inline bool is_stale_nan(double v) {
	return is_stale(v);
}

// `docs/testing/comparator.md`'s own shown derivation, transcribed verbatim.
// `scale` is `Σ|terms|`, or whatever quantity a row's `scale_kind`
// (`src/kernel/registry_types.hpp:ScaleKind:` `enum class ScaleKind {`) names; the
// bound is absolute in the answer's units, never relative, so a cancelling
// fold with a small true answer and a large `Σ|terms|` is never rejected
// for a reason that has nothing to do with its correctness.
inline bool equal_values(double a, double b, double scale) {
	if (is_stale_nan(a) && is_stale_nan(b))
		return true; // stale marker is its own value
	if (std::isnan(a) && std::isnan(b))
		return true; // NaN is a legitimate answer
	if (std::isnan(a) || std::isnan(b))
		return false;
	if (std::isinf(a) || std::isinf(b))
		return a == b; // infinities agree only bit-identically
	if (a == b)
		return true;
	return std::fabs(a - b) <= kReorderFactor * scale;
}

// The L13 "comparator headroom pin" — not a second tolerance, a margin
// check on the one above. `docs/testing/comparator.md` pins two real
// reference points: the largest drift any translation-only implementation
// has ever legitimately produced
// (`docs/testing/comparator.md:headroom-accept:` `1–5 ULP`, "three to four
// orders inside the bound") and the smallest real divergence the fence has
// ever caught, the duplicate-timestamp bug at 3×10⁻²
// (`docs/testing/comparator.md:headroom-reject:` `ten orders outside`).
// `accept_orders`/`reject_orders` below are how many orders of magnitude of
// headroom currently separate `reorder_factor` from each edge;
// `test/kernel/comparator_test.cpp` fails
// if either has shrunk below the floor the document itself states — three
// and ten orders respectively — since a fence that no longer clears its own
// stated headroom has moved toward reproducing one of the two failures the
// derivation exists to avoid.
struct HeadroomMargins {
	double accept_orders; // reorder_factor / (5 * unit_roundoff)
	double reject_orders; // 0.03 / reorder_factor
};

constexpr HeadroomMargins ReorderFactorHeadroom(double reorder_factor, double unit_roundoff) {
	constexpr double kLargestAcceptedDriftUlps = 5.0;    // "1-5 ULP", the worse of the two
	constexpr double kSmallestRejectedDivergence = 3e-2; // the duplicate-timestamp bug
	return HeadroomMargins {
	    reorder_factor / (kLargestAcceptedDriftUlps * unit_roundoff),
	    kSmallestRejectedDivergence / reorder_factor,
	};
}

} // namespace chronoduck
