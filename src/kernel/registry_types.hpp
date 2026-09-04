// registry_types.hpp — enums, static rules and the family→layers map behind
// src/kernel/registry.def (Article V.1). Deliberately dependency-free: no
// `#include "duckdb.hpp"` and no other DuckDB header, so this file compiles
// standalone with a bare `g++`/`clang++ -std=c++17` and can be compile-tested
// by scripts/hygiene-selftest.mjs's fixtures without the duckdb submodule
// checked out.
#pragma once

#include <cstddef>

namespace chronoduck {

// The kernel's per-row family. `docs/design/surface.md` defines RANGE,
// INSTANT, HIST, GRID, SERIES and PATTERN as the fold families derived from
// walking the three reference consumers. META is this project's own
// addition, beyond that design doc: it exists so a niladic, non-kernel,
// extension-level metadata function (`chronoduck_version`) has a family to
// register under at all — it is not a per-series kernel fold, so it would
// otherwise have no honest row and Article V.1 ("a function exists only
// through registry.def") would have a gap for it.
enum class Family { RANGE, INSTANT, HIST, GRID, SERIES, PATTERN, META };

// The kernel's per-row state shape — `docs/design/surface.md:layers:` `the family → layers map` names
// the same "state" column this enum encodes (RAW_WINDOW, SLICE, LAST_K, HIST_WINDOW, HIST_MERGE,
// SKETCH, SERIES and NFA there). NONE stands in for that table's `—` sentinel: a stateless scalar
// function, which is what both of this PR's rows (`chronoduck_version`, `ts_grid_index`) are.
enum class StateClass { RAW_WINDOW, SLICE, LAST_K, HIST_WINDOW, HIST_MERGE, SKETCH, SERIES_STATE, NFA, NONE };

// The kernel's determinism classes, `docs/design/surface.md`'s `det` column:
// D0 bit-identical under any partition, D1 within the reorder bound, D2(ε)
// within a documented sketch bound.
enum class Determinism { D0, D1, D2 };

// Edge-reading modes a RANGE/INSTANT/HIST row can combine (bitmask — a row
// can support more than one). Both of this PR's rows are stateless scalars
// with no window to read edges of, so both use EDGE_NONE.
enum EdgeMode : unsigned {
	EDGE_NONE = 0,
	INSIDE = 1u << 0,
	EXTRAPOLATE = 1u << 1,
	ANCHOR = 1u << 2,
	SMOOTH = 1u << 3,
	LOOKBACK = 1u << 4,
	INTERPOLATE = 1u << 5,
	NEXT = 1u << 6,
	NEAREST = 1u << 7,
};

// Value-domain constraints a row's input/output is checked against (bitmask).
// Both of this PR's rows are domain-agnostic (a version string, an integer
// grid index), so both use DOMAIN_NONE.
enum Domain : unsigned {
	DOMAIN_NONE = 0,
	COUNTER = 1u << 0,
	GAUGE = 1u << 1,
	NONNEG = 1u << 2,
	ANY = 1u << 3,
	HISTOGRAM = 1u << 4,
	CATEGORICAL = 1u << 5,
};

// The comparator's conditioning class for a row, `docs/testing/comparator.md`'s
// scale_kind column (see its rationale where `IsValidRow` is defined below).
enum class ScaleKind {
	EXACT,
	SUM_ABS,
	SUM_ABS_TIMES_FACTOR,
	RESIDUAL_SS,
	SLOPE_COND,
	LIBM,
	LIBM_EXP,
	NORM2_LOGN,
	RECURRENCE,
};

// Both of this PR's rows use ScaleKind::EXACT:
//   - `ts_grid_index` is integer floor division (see
//     `src/chronoduck_extension.cpp:FloorDiv:` `__int128_t quotient`) —
//     always bit-exact, so EXACT is the only honest scale_kind.
//   - `chronoduck_version`'s output is a fixed string with no numeric tolerance to compare against;
//     EXACT (scale 0, bit-exact) is vacuously correct for it — see
//     `docs/testing/comparator.md:EXACT:` `(scale 0, bit-exact) for every selection` — a string
//     equality has no "scale" to be anything other than exact.

// IsValidRow — the mechanically checkable half of the rule below, see
// `docs/design/surface.md:static-rule:` `any state whose partial contains a float sum is D1 unless it uses
// reproducible_sum — applies to SLICE, HIST_MERGE and SKETCH alike` There is no `reproducible_sum` column in this
// 7-column schema yet, so no row can claim that exception: a future issue that implements reproducible summation
// extends this function then, rather than a row silently claiming D0 today.
//
// Deliberate, documented limitation: this function does NOT check issue #26's Goal's second static
// rule ("D2 only for SKETCH rows whose output carries [lo, hi]" — this exact wording is the issue's,
// not `docs/design/surface.md`'s own text) for a row like this one, see
// `docs/design/surface.md:approx_top_k_over_time:` `count_lo, count_hi`
// a SKETCH+D2 row whose output is `LIST(STRUCT(key, count_lo, count_hi))`. Checking that half needs an
// output-shape signal — whether a row's result type carries a [lo, hi] pair —
// that this 7-column (name, family, state, det, edge_modes, domain,
// scale_kind) schema has no column for. So `IsValidRow` returns `true` for
// every SKETCH row with `det == Determinism::D2`, including one whose output
// does NOT carry [lo, hi]; that obligation remains a review-time/fixture-time
// (L2/L12) responsibility until a future column or row-tag adds the signal
// this function would need to check it at compile time. This is a known gap
// in this PR's static checking, not an oversight — the L1 half of the rule
// (the float-sum-implies-D1 check below) is fully mechanical and IS checked.
constexpr bool IsValidRow(Family family, StateClass state, Determinism det, ScaleKind scale) {
	return (void)family,
	       !((state == StateClass::SLICE || state == StateClass::HIST_MERGE || state == StateClass::SKETCH) &&
	         (scale == ScaleKind::SUM_ABS || scale == ScaleKind::SUM_ABS_TIMES_FACTOR) && det != Determinism::D1);
}

// The family → layers map — `docs/design/surface.md:layers:` `the family → layers map` — this is its
// one home; registry.def's header points here rather than duplicating this data. Presence-listing
// only (L13 iterates this map to check each layer's artifact exists for a row's family) — not a
// per-layer applicability judgment, so a layer vacuous for a particular row (L7's histogram oracles
// for a non-HIST row, say) is still listed here.
//
// RANGE, INSTANT, HIST and GRID share the full L0–L14 map. Kept uniform
// across all four rather than carving out family-specific exceptions (e.g.
// L7 is vacuous for non-HIST families) — simplicity over precision, since
// this map's job is presence-listing, not applicability judgment.
constexpr const char *const kLayersRangeInstantHistGrid[] = {"L0", "L1a", "L1b", "L1c", "L2",  "L3",
                                                             "L4", "L5",  "L6",  "L6a", "L7",  "L8",
                                                             "L9", "L10", "L11", "L12", "L13", "L14"};

// SERIES: a scalar over a regular series value (LIST(DOUBLE) plus the grid
// descriptor), not an edge-reading fold — no L1c/L4/L6-only edge machinery
// applies to it the same way, hence the narrower list and the primed L4'/L11'
// entries: bit-identity across build/thread/partition variance, and
// `docs/design/surface.md:layers:` `temporary-allocation law` for L11' — not
// "memory-allocation law" as an earlier draft mistakenly renamed it to dodge
// a forbid-deferral false positive on "temporary" (#184); the citation is
// exact instead. See that comment in surface.md for what each primed entry
// means rather than restating it here.
constexpr const char *const kLayersSeries[] = {"L0", "L1a", "L1b", "L1c", "L2",  "L3",  "L5",  "L6a",
                                               "L9", "L10", "L12", "L13", "L14", "L4'", "L11'"};

// PATTERN: identical to SERIES for this map's granularity (row-pattern
// recognition adds the NFA fuzz lane and MR-PATTERN-PARTITION, which this
// presence-listing manifest does not need a separate literal list to
// represent, since it names no layer SERIES doesn't already carry).
constexpr const char *const kLayersPattern[] = {"L0", "L1a", "L1b", "L1c", "L2",  "L3",  "L5",  "L6a",
                                                "L9", "L10", "L12", "L13", "L14", "L4'", "L11'"};

// META: this project's own addition (see the Family comment above). A
// registry-closure function is not a per-series kernel primitive, so the
// numerics/determinism/histogram/memory/mutation machinery of L1-L4 and
// L6-L14 doesn't apply to it at all — only registry closure (L0) and a
// sqllogictest (L5) do.
constexpr const char *const kLayersMeta[] = {"L0", "L5"};

struct FamilyLayers {
	Family family;
	const char *const *layers;
	std::size_t count;
};

template <std::size_t N>
constexpr FamilyLayers MakeFamilyLayers(Family family, const char *const (&layers)[N]) {
	return FamilyLayers {family, layers, N};
}

constexpr FamilyLayers kFamilyLayersMap[] = {
    MakeFamilyLayers(Family::RANGE, kLayersRangeInstantHistGrid),
    MakeFamilyLayers(Family::INSTANT, kLayersRangeInstantHistGrid),
    MakeFamilyLayers(Family::HIST, kLayersRangeInstantHistGrid),
    MakeFamilyLayers(Family::GRID, kLayersRangeInstantHistGrid),
    MakeFamilyLayers(Family::SERIES, kLayersSeries),
    MakeFamilyLayers(Family::PATTERN, kLayersPattern),
    MakeFamilyLayers(Family::META, kLayersMeta),
};
constexpr std::size_t kFamilyLayersMapCount = sizeof(kFamilyLayersMap) / sizeof(kFamilyLayersMap[0]);

// The static_assert generator for registry.def's (state, det, scale_kind)
// rule, run once per translation unit that includes this header (Article
// V.1: "A row missing from any roster fails the build, named after the
// row" — here, a row whose combination is invalid fails the build, named
// after the row). registry.def redefines TS_FN as a no-op when nothing else
// has defined it first (its own `#ifndef TS_FN` guard) and undefines it
// again at the end of the file, so this inclusion is self-contained and
// leaves no macro behind for whatever includes registry_types.hpp next.
namespace registry_static_checks {
#define TS_FN(name, family, state, det, edge, domain, scale)                                                           \
	static_assert(::chronoduck::IsValidRow(::chronoduck::Family::family, ::chronoduck::StateClass::state,              \
	                                       ::chronoduck::Determinism::det, ::chronoduck::ScaleKind::scale),            \
	              "invalid (state, det, scale_kind) combination for row '" #name "'");
#include "registry.def"
} // namespace registry_static_checks

} // namespace chronoduck
