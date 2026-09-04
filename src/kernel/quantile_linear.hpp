// quantile_linear.hpp — quantile_linear, a Tier 0 numeric primitive
// (`docs/design/primitives.md:tier0-row-quantile:` `quantile_linear`).
// The Hyndman-Fan Type 7 / R7 rank convention `docs/design/surface.md`'s
// `quantile_over_time` row names as this kernel's shared quantile
// convention (`docs/design/surface.md:quantile-over-time-row:` `R7 linear is the default and the convention the oracles
// share`). Deliberately dependency-free: no `#include "duckdb.hpp"`, compiles standalone with a bare `g++`/`clang++
// -std=c++17` — the pattern `comparator.hpp`, `kahan.hpp` and `linear_regression.hpp` established for Article V.1's
// TU-per-primitive rule.
//
// This primitive does not sort: sorting is Tier 2's job
// (`docs/design/primitives.md`'s `SampleBuffer`'s `sort_dedup`), so the
// caller passes an already-ascending-sorted array — the name says so, and
// the precondition is documented rather than re-checked at this layer.
#pragma once

#include <cstddef>
#include <cmath>

namespace chronoduck {

// `sorted` must have `n` elements in ascending order; `phi` must lie in
// `[0, 1]` — both are the caller's precondition (bind-time validation is a
// higher tier's job), not this function's.
inline double quantile_linear(const double *sorted, std::size_t n, double phi) {
	if (n == 0) {
		return std::nan("");
	}
	if (n == 1) {
		return sorted[0];
	}
	// The rank: `docs/testing/primitives.md`'s must-die mutant "Rank
	// rounding" is exactly `std::round(rank)` in place of the floor/ceil
	// pair below — that would collapse every interpolated case onto its
	// nearer neighbour instead of blending the two by `weight`.
	const double rank = phi * static_cast<double>(n - 1);
	const double rank_floor = std::floor(rank);
	const double rank_ceil = std::ceil(rank);
	const std::size_t lower_index = static_cast<std::size_t>(rank_floor);
	const std::size_t upper_index = static_cast<std::size_t>(rank_ceil);
	const double weight = rank - rank_floor;
	// The must-die mutant "interpolation weight inversion" is `weight` and
	// `1.0 - weight` swapped between the two terms below — correct at the
	// two rank-exactly-on-an-element cases (weight 0 or 1, where the wrong
	// term is multiplied by zero and the bug is invisible) and wrong
	// everywhere the rank falls strictly between two elements.
	return sorted[lower_index] * (1.0 - weight) + sorted[upper_index] * weight;
}

} // namespace chronoduck
