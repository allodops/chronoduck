// quantile_linear_test.cpp — the L1a direct test for
// `src/kernel/quantile_linear.hpp` (docs/testing/layers.md's L1a row:
// "every Tier 0-5 primitive has its own translation unit, its own
// table-driven tests ... exercised directly"). Hand-rolled `main()`, no
// test framework, compiled and run with a bare `g++ -std=c++17` by
// `scripts/hygiene/kernel-primitive-tests.mjs`.
//
// Follows `docs/testing/primitives.md`'s Tier 0 `quantile_linear` row: the
// φ × n table, the three invariants, the sorted-vector oracle written out
// independently, and both must-die mutants, each demonstrated by a shadow
// implementation carrying the mutation.
#include "kernel/quantile_linear.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using chronoduck::quantile_linear;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		std::fprintf(stderr, "quantile_linear_test: FAIL — %s\n", what);
		g_failures++;
	}
}

// The independent oracle: `docs/testing/primitives.md`'s "A sorted-vector
// reference with the reference's rank convention written out" — the R7/
// Hyndman-Fan rank formula spelled out by hand on a `std::vector`, as an
// independent re-derivation rather than a call into the header under test.
double SortedVectorOracle(const std::vector<double> &sorted, double phi) {
	std::size_t n = sorted.size();
	if (n == 0)
		return std::nan("");
	if (n == 1)
		return sorted[0];
	double rank = phi * static_cast<double>(n - 1);
	std::size_t lo = static_cast<std::size_t>(rank); // truncation toward zero == floor for rank >= 0
	std::size_t hi = (lo + 1 < n) ? lo + 1 : lo;
	double weight = rank - static_cast<double>(lo);
	return sorted[lo] + weight * (sorted[hi] - sorted[lo]); // a differently-shaped, algebraically equal formula
}

struct Case {
	const char *name;
	std::vector<double> sorted;
	double phi;
};

// Unit contract: "φ ∈ {0, 0.5, 1, 0.99} on n ∈ {1, 2, 3, 100}; rank exactly
// on an element; rank between elements"
// (`docs/testing/primitives.md:quantile-linear-contract:` `φ ∈ {0, 0.5, 1, 0.99} on n ∈ {1, 2, 3, 100}`).
std::vector<double> Ascending(std::size_t n) {
	std::vector<double> xs;
	for (std::size_t i = 0; i < n; i++)
		xs.push_back(static_cast<double>(i) + 1.0); // 1, 2, ..., n
	return xs;
}

void TestTableAgainstOracle() {
	std::vector<std::size_t> ns = {1, 2, 3, 100};
	std::vector<double> phis = {0.0, 0.5, 1.0, 0.99};
	for (std::size_t n : ns) {
		std::vector<double> sorted = Ascending(n);
		for (double phi : phis) {
			double expected = SortedVectorOracle(sorted, phi);
			double actual = quantile_linear(sorted.data(), n, phi);
			char what[256];
			std::snprintf(what, sizeof(what), "n=%zu phi=%.2f: quantile_linear (%.17g) must match the oracle (%.17g)",
			              n, phi, actual, expected);
			Check(actual == expected, what);
		}
	}
}

// Unit contract: "rank exactly on an element" (phi chosen so `rank` is a
// whole number) and "rank between elements" (phi chosen so it isn't),
// spelled out explicitly rather than only incidentally covered by the table
// above.
void TestRankExactlyOnElement() {
	std::vector<double> xs = {10.0, 20.0, 30.0, 40.0, 50.0}; // n=5, rank = phi*4
	Check(quantile_linear(xs.data(), 5, 0.25) == 20.0,
	      "rank exactly on element 1 (phi=0.25, rank=1.0) must return that element exactly");
	Check(quantile_linear(xs.data(), 5, 0.75) == 40.0,
	      "rank exactly on element 3 (phi=0.75, rank=3.0) must return that element exactly");
}

void TestRankBetweenElements() {
	std::vector<double> xs = {0.0, 10.0}; // n=2, rank = phi
	Check(quantile_linear(xs.data(), 2, 0.5) == 5.0,
	      "rank between elements 0 and 1 (phi=0.5) must interpolate halfway");
	Check(quantile_linear(xs.data(), 2, 0.25) == 2.5,
	      "rank between elements 0 and 1 (phi=0.25) must interpolate a quarter of the way");
}

// Invariant: "Monotone in φ".
void TestMonotoneInPhi() {
	std::vector<double> xs = {3.0, 1.0, 4.0, 1.0, 5.0, 9.0, 2.0, 6.0};
	std::sort(xs.begin(), xs.end());
	double previous = quantile_linear(xs.data(), xs.size(), 0.0);
	for (int i = 1; i <= 100; i++) {
		double phi = static_cast<double>(i) / 100.0;
		double current = quantile_linear(xs.data(), xs.size(), phi);
		Check(current >= previous, "quantile_linear must be monotone non-decreasing in phi");
		previous = current;
	}
}

// Invariant: "φ=0 is min, φ=1 is max; result lies in [min, max]".
void TestBoundaryAndRange() {
	std::vector<double> xs = {-5.0, 2.0, 3.5, 100.0, -20.0};
	std::sort(xs.begin(), xs.end());
	double min_v = xs.front(), max_v = xs.back();
	Check(quantile_linear(xs.data(), xs.size(), 0.0) == min_v, "phi=0 must be the minimum");
	Check(quantile_linear(xs.data(), xs.size(), 1.0) == max_v, "phi=1 must be the maximum");
	for (double phi : {0.0, 0.1, 0.33, 0.5, 0.75, 0.9, 1.0}) {
		double v = quantile_linear(xs.data(), xs.size(), phi);
		Check(v >= min_v && v <= max_v, "quantile_linear's result must lie in [min, max]");
	}
}

// Must-die mutant #1: "Rank rounding"
// (`docs/testing/primitives.md:quantile-linear-mutants:` `Rank rounding`).
// `std::round` in place of the floor/ceil pair — invisible when the rank
// already lands on an integer, wrong for every interpolated case.
double MutantRankRounding(const double *sorted, std::size_t n, double phi) {
	if (n == 0)
		return std::nan("");
	if (n == 1)
		return sorted[0];
	double rank = phi * static_cast<double>(n - 1);
	std::size_t rounded = static_cast<std::size_t>(std::round(rank));
	return sorted[rounded]; // rounds to the nearer element instead of interpolating
}

void TestRankRoundingMutant() {
	std::vector<double> xs = {0.0, 10.0}; // n=2, rank = phi; the true quantile at phi=0.5 interpolates to 5.0
	double real_answer = quantile_linear(xs.data(), 2, 0.5);
	double mutant_answer = MutantRankRounding(xs.data(), 2, 0.5);
	Check(real_answer == 5.0, "sanity: the real primitive interpolates to 5.0 at phi=0.5 on {0, 10}");
	Check(mutant_answer != real_answer,
	      "must-die: rounding the rank instead of interpolating must diverge from the true interpolated answer");
}

// Must-die mutant #2: "interpolation weight inversion"
// (`docs/testing/primitives.md:quantile-linear-mutants:` `interpolation weight inversion`).
// `weight` and `1 - weight` swapped between the two terms.
double MutantWeightInversion(const double *sorted, std::size_t n, double phi) {
	if (n == 0)
		return std::nan("");
	if (n == 1)
		return sorted[0];
	double rank = phi * static_cast<double>(n - 1);
	double rank_floor = std::floor(rank);
	double rank_ceil = std::ceil(rank);
	std::size_t lo = static_cast<std::size_t>(rank_floor);
	std::size_t hi = static_cast<std::size_t>(rank_ceil);
	double weight = rank - rank_floor;
	return sorted[lo] * weight + sorted[hi] * (1.0 - weight); // the two weights swapped
}

void TestWeightInversionMutant() {
	// phi=0 and phi=1 are the exact-rank cases where this mutation is
	// invisible (one weight is exactly 0); phi strictly between them, on a
	// non-symmetric array, is where the swap actually shows up.
	std::vector<double> xs = {0.0, 100.0}; // n=2, rank = phi, strongly asymmetric
	double real_answer = quantile_linear(xs.data(), 2, 0.25);
	double mutant_answer = MutantWeightInversion(xs.data(), 2, 0.25);
	Check(real_answer == 25.0, "sanity: the real primitive gives 25.0 at phi=0.25 on {0, 100}");
	Check(mutant_answer != real_answer,
	      "must-die: swapping the interpolation weights must diverge from the true interpolated answer");

	// And confirm the mutation really is invisible exactly at the two
	// boundary phis, so this test could not have been satisfied by
	// accident at those values alone.
	Check(MutantWeightInversion(xs.data(), 2, 0.0) == quantile_linear(xs.data(), 2, 0.0),
	      "sanity: the weight-inversion mutant is (as expected) invisible at phi=0");
	Check(MutantWeightInversion(xs.data(), 2, 1.0) == quantile_linear(xs.data(), 2, 1.0),
	      "sanity: the weight-inversion mutant is (as expected) invisible at phi=1");
}

} // namespace

int main() {
	TestTableAgainstOracle();
	TestRankExactlyOnElement();
	TestRankBetweenElements();
	TestMonotoneInPhi();
	TestBoundaryAndRange();
	TestRankRoundingMutant();
	TestWeightInversionMutant();

	if (g_failures > 0) {
		std::fprintf(stderr, "quantile_linear_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("quantile_linear_test: PASS\n");
	return 0;
}
