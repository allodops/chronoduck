// linear_regression_test.cpp — the L1a direct test for
// `src/kernel/linear_regression.hpp` (docs/testing/layers.md's L1a row:
// "every Tier 0-5 primitive has its own translation unit, its own
// table-driven tests ... exercised directly"). Hand-rolled `main()`, no
// test framework, compiled and run with a bare `g++ -std=c++17` by
// `scripts/hygiene/kernel-primitive-tests.mjs`.
//
// Follows `docs/testing/primitives.md`'s Tier 0 `linear_regression` row:
// the table (two points, collinear, constant series), the three invariants,
// the extended-precision oracle, and both must-die mutants, each
// demonstrated by a shadow implementation carrying the mutation.
#include "kernel/comparator.hpp"
#include "kernel/linear_regression.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using chronoduck::equal_values;
using chronoduck::kUnitRoundoff;
using chronoduck::linear_regression;
using chronoduck::LinearFit;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		std::fprintf(stderr, "linear_regression_test: FAIL — %s\n", what);
		g_failures++;
	}
}

// The independent oracle: `docs/testing/primitives.md`'s "Closed-form least
// squares in extended precision" — the identical algebraic formula,
// re-derived here in `long double` rather than reused from the header, so a
// bug shared by both the production `double` path and a copy-pasted test
// helper cannot hide. `long double` is 80-bit extended precision on this
// platform, well beyond `double`'s 53-bit mantissa for the modest sample
// counts this table uses.
LinearFit OracleFit(const std::vector<double> &t, const std::vector<double> &v, double origin) {
	long double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
	std::size_t n = t.size();
	for (std::size_t i = 0; i < n; i++) {
		long double x = static_cast<long double>(t[i]) - static_cast<long double>(origin);
		long double y = v[i];
		sum_x += x;
		sum_y += y;
		sum_xy += x * y;
		sum_x2 += x * x;
	}
	long double count = static_cast<long double>(n);
	long double cov_xy = sum_xy - sum_x * sum_y / count;
	long double var_x = sum_x2 - sum_x * sum_x / count;
	long double slope = cov_xy / var_x;
	long double intercept = sum_y / count - slope * sum_x / count;
	return LinearFit {static_cast<double>(slope), static_cast<double>(intercept)};
}

// A tight bound scaled by the fit's own magnitude — `docs/testing/comparator.md`'s
// SLOPE_COND scale kind is for cross-implementation agreement on the fold's
// *output*, not for judging whether a conditioning bug destroyed the
// computation (the same reasoning `kahan_test.cpp`'s `CancellationBound`
// documents for its own primitive).
double FitBound(double reference_value) {
	return 64.0 * kUnitRoundoff * std::max(1.0, std::fabs(reference_value));
}

struct Table {
	const char *name;
	std::vector<double> t;
	std::vector<double> v;
	double origin;
};

// Unit contract: "two points; collinear; constant series (slope 0)".
const Table kTable[] = {
    {"two points", {0.0, 10.0}, {1.0, 21.0}, 0.0},                                         // slope 2, intercept 1
    {"collinear, five points", {0.0, 1.0, 2.0, 3.0, 4.0}, {1.0, 3.0, 5.0, 7.0, 9.0}, 0.0}, // slope 2, intercept 1
    {"constant series (slope 0)", {5.0, 6.0, 7.0, 8.0}, {3.0, 3.0, 3.0, 3.0}, 0.0},
    {"negative slope, nonzero origin", {100.0, 101.0, 102.0}, {50.0, 47.0, 44.0}, 100.0}, // slope -3
};

void TestTableAgainstOracle() {
	for (const auto &c : kTable) {
		LinearFit oracle = OracleFit(c.t, c.v, c.origin);
		LinearFit actual = linear_regression(c.t.data(), c.v.data(), c.t.size(), c.origin);
		char what[256];
		std::snprintf(what, sizeof(what), "%s: slope %.17g must match the oracle's %.17g", c.name, actual.slope,
		              oracle.slope);
		Check(std::fabs(actual.slope - oracle.slope) <= FitBound(oracle.slope), what);
		std::snprintf(what, sizeof(what), "%s: intercept %.17g must match the oracle's %.17g", c.name, actual.intercept,
		              oracle.intercept);
		Check(std::fabs(actual.intercept - oracle.intercept) <= FitBound(oracle.intercept), what);
	}
}

// Unit contract edge: n < 2 has no defined slope.
void TestTooFewPoints() {
	double t[1] = {0.0};
	double v[1] = {5.0};
	LinearFit zero_points = linear_regression(nullptr, nullptr, 0, 0.0);
	LinearFit one_point = linear_regression(t, v, 1, 0.0);
	Check(std::isnan(zero_points.slope) && std::isnan(zero_points.intercept), "zero points must return NaN, NaN");
	Check(std::isnan(one_point.slope) && std::isnan(one_point.intercept), "one point must return NaN, NaN");
}

// Invariant: "residual of an exact line is 0" — every sample lies exactly on
// v = m*t + b, so the fit must recover m and b (at the chosen origin) with
// zero residual at every sample point.
void TestExactLineZeroResidual() {
	const double m = -2.5, b = 7.0, origin = 3.0;
	std::vector<double> t = {0.0, 1.0, 2.0, 3.0, 4.0, 5.0};
	std::vector<double> v;
	for (double x : t)
		v.push_back(m * x + b);

	LinearFit fit = linear_regression(t.data(), v.data(), t.size(), origin);
	Check(equal_values(fit.slope, m, std::fabs(m)), "exact line: recovered slope must match the line's true slope");

	for (std::size_t i = 0; i < t.size(); i++) {
		double predicted = fit.slope * (t[i] - origin) + fit.intercept;
		double residual = v[i] - predicted;
		Check(std::fabs(residual) <= FitBound(v[i]), "exact line: residual at every sample must be ~0");
	}
}

// Invariant: "Shift-invariant in t" — the slope must not depend on the
// chosen origin, and the intercept at a different origin must equal the
// intercept at the first origin plus slope times the origin's own shift
// (the fitted line evaluated at a different point).
void TestShiftInvariantInT() {
	std::vector<double> t = {1000.0, 1005.0, 1013.0, 1024.0, 1030.0};
	std::vector<double> v = {12.0, 15.5, 21.0, 27.0, 30.5};

	LinearFit at_zero = linear_regression(t.data(), v.data(), t.size(), 0.0);
	LinearFit at_first_sample = linear_regression(t.data(), v.data(), t.size(), t[0]);

	Check(equal_values(at_zero.slope, at_first_sample.slope, std::fabs(at_zero.slope)),
	      "shift-invariant in t: slope must not depend on the chosen origin");

	double expected_intercept_shift = at_zero.slope * (t[0] - 0.0);
	double predicted_intercept_at_first_sample = at_zero.intercept + expected_intercept_shift;
	Check(std::fabs(at_first_sample.intercept - predicted_intercept_at_first_sample) <= FitBound(at_zero.intercept),
	      "shift-invariant in t: intercept at a new origin must equal the old fit evaluated there");
}

// Invariant: "scale-equivariant in v" — scaling every v by a constant
// factor scales both slope and intercept by that same factor.
void TestScaleEquivariantInV() {
	std::vector<double> t = {0.0, 2.0, 4.0, 6.0, 9.0};
	std::vector<double> v = {1.0, 1.5, 3.0, 3.5, 6.0};
	const double factor = 4.0;
	std::vector<double> scaled_v;
	for (double x : v)
		scaled_v.push_back(x * factor);

	LinearFit base = linear_regression(t.data(), v.data(), t.size(), 0.0);
	LinearFit scaled = linear_regression(t.data(), scaled_v.data(), t.size(), 0.0);

	Check(equal_values(scaled.slope, base.slope * factor, std::fabs(base.slope * factor)),
	      "scale-equivariant in v: slope must scale by the same factor as v");
	Check(equal_values(scaled.intercept, base.intercept * factor, std::fabs(base.intercept * factor)),
	      "scale-equivariant in v: intercept must scale by the same factor as v");
}

// Must-die mutant #1: "Origin shift removed (conditioning)"
// (`docs/testing/primitives.md:linreg-mutant-origin:` `Origin shift removed (conditioning)`).
// The exact same closed-form sums, computed directly from `t` with no shift
// at all — the mutant this header's own comment names.
LinearFit MutantNoOriginShift(const double *t, const double *v, std::size_t n) {
	double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
	for (std::size_t i = 0; i < n; i++) {
		sum_x += t[i];
		sum_y += v[i];
		sum_xy += t[i] * v[i];
		sum_x2 += t[i] * t[i];
	}
	double count = static_cast<double>(n);
	double cov_xy = sum_xy - sum_x * sum_y / count;
	double var_x = sum_x2 - sum_x * sum_x / count;
	double slope = cov_xy / var_x;
	double intercept = sum_y / count - slope * sum_x / count;
	return LinearFit {slope, intercept};
}

void TestOriginShiftMutant() {
	// Microsecond-epoch-scale timestamps (~1.7e15, the reference's own
	// timestamp domain) with a small, exactly-representable slope — the
	// shape that makes `Σt²` and `(Σt)²/n` nearly cancel without a shift.
	std::vector<double> t;
	std::vector<double> v;
	const double base = 1.7e15;
	for (int i = 0; i < 10; i++) {
		t.push_back(base + i * 1000.0);
		v.push_back(1.0 + 0.001 * (i * 1000.0));
	}
	const double true_slope = 0.001;

	LinearFit real_fit = linear_regression(t.data(), v.data(), t.size(), t[0]);
	LinearFit mutant_fit = MutantNoOriginShift(t.data(), v.data(), t.size());

	Check(std::fabs(real_fit.slope - true_slope) <= FitBound(true_slope),
	      "origin-shifted linear_regression must recover the true slope at microsecond-epoch scale");
	Check(std::fabs(mutant_fit.slope - true_slope) > 1e-6,
	      "must-die: removing the origin shift must observably destroy the slope at microsecond-epoch scale — "
	      "this mutant is caught by exactly the conditioning case the shift exists for");
}

// Must-die mutant #2: the sample count used inconsistently
// (`docs/testing/primitives.md`'s linear_regression row, the other named
// must-die mutant: the sample count swapped for one less than itself in
// exactly one of the three divisions this formula performs).
LinearFit MutantInconsistentCount(const double *t, const double *v, std::size_t n, double origin) {
	double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
	for (std::size_t i = 0; i < n; i++) {
		double x = t[i] - origin;
		sum_x += x;
		sum_y += v[i];
		sum_xy += x * v[i];
		sum_x2 += x * x;
	}
	double count = static_cast<double>(n);
	double count_minus_one = count - 1.0; // the mutation: used below where `count` belongs
	double cov_xy = sum_xy - sum_x * sum_y / count;
	double var_x = sum_x2 - sum_x * sum_x / count_minus_one; // mutated divisor
	double slope = cov_xy / var_x;
	double intercept = sum_y / count - slope * sum_x / count;
	return LinearFit {slope, intercept};
}

void TestSampleCountMutant() {
	std::vector<double> t = {0.0, 1.0, 2.0, 3.0, 4.0};
	std::vector<double> v = {1.0, 3.0, 5.0, 7.0, 9.0}; // exact line: slope 2, intercept 1
	const double true_slope = 2.0;

	LinearFit real_fit = linear_regression(t.data(), v.data(), t.size(), 0.0);
	LinearFit mutant_fit = MutantInconsistentCount(t.data(), v.data(), t.size(), 0.0);

	Check(std::fabs(real_fit.slope - true_slope) <= FitBound(true_slope),
	      "linear_regression must recover the true slope on an exact line");
	Check(std::fabs(mutant_fit.slope - true_slope) > FitBound(true_slope) * 1000.0,
	      "must-die: using the sample count minus one in exactly one division must observably diverge from the true "
	      "slope");
}

} // namespace

int main() {
	TestTableAgainstOracle();
	TestTooFewPoints();
	TestExactLineZeroResidual();
	TestShiftInvariantInT();
	TestScaleEquivariantInV();
	TestOriginShiftMutant();
	TestSampleCountMutant();

	if (g_failures > 0) {
		std::fprintf(stderr, "linear_regression_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("linear_regression_test: PASS\n");
	return 0;
}
