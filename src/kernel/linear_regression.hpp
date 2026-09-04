// linear_regression.hpp — linear_regression, a Tier 0 numeric primitive
// (`docs/design/primitives.md:tier0-row-linreg:` `linear_regression`
// (origin-shifted least squares)). Serves `deriv` (least-squares slope,
// `docs/design/surface.md:deriv-row:` `least-squares slope`) and
// `predict_linear` (`docs/design/surface.md:predict-linear-row:` `slope + intercept at t + horizon`).
// Deliberately dependency-free: no `#include "duckdb.hpp"`, compiles
// standalone with a bare `g++`/`clang++ -std=c++17` — the pattern
// `comparator.hpp` and `kahan.hpp` established for Article V.1's
// TU-per-primitive rule.
//
// Origin-shifted: every `t` is shifted by a caller-supplied `origin` before
// the closed-form sums are formed. This is not a stylistic choice — the
// reference's own timestamps are large absolute values (microseconds since
// the epoch, ~1.7e15 and growing), and forming `Σt²` directly from such
// values before subtracting a comparably large `(Σt)²/n` cancels almost
// every significant digit the slope needs (the must-die mutant
// `docs/testing/primitives.md:linreg-mutant-origin:` `Origin shift removed (conditioning)`
// this primitive's own test file demonstrates numerically). Shifting first
// keeps every sum near the scale of the data's actual spread.
#pragma once

#include <cstddef>
#include <limits>

namespace chronoduck {

// `slope` is shift-invariant; `intercept` is the fitted line's value at
// `t == origin` (not at `t == 0`) — the caller picks `origin` and reads
// `intercept` as "value at origin", exactly as `predict_linear` needs
// "value at the query time" and `deriv` needs "value at the range start".
struct LinearFit {
	double slope;
	double intercept;
};

// n < 2 has no defined slope (the reference's own `deriv`/`predict_linear`
// return no value for a single sample) — NaN in both fields, never a
// fabricated 0.
inline LinearFit linear_regression(const double *t, const double *v, std::size_t n, double origin) {
	if (n < 2) {
		const double nan_value = std::numeric_limits<double>::quiet_NaN();
		return LinearFit {nan_value, nan_value};
	}

	double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_x2 = 0.0;
	for (std::size_t i = 0; i < n; i++) {
		const double x = t[i] - origin;
		sum_x += x;
		sum_y += v[i];
		sum_xy += x * v[i];
		sum_x2 += x * x;
	}

	const double count = static_cast<double>(n);
	// Every occurrence of `count` below divides by the *same* n — the
	// linear_regression row's other must-die mutant in
	// `docs/testing/primitives.md` (swapping the sample count for the
	// sample count minus one in exactly one of the three divisions; see the
	// test file's demonstration) is exactly what changing any one of these
	// three would introduce.
	const double cov_xy = sum_xy - sum_x * sum_y / count;
	const double var_x = sum_x2 - sum_x * sum_x / count;
	const double slope = cov_xy / var_x;
	const double intercept = sum_y / count - slope * sum_x / count;
	return LinearFit {slope, intercept};
}

} // namespace chronoduck
