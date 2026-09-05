// comparator_test.cpp — the L1a direct test for `src/kernel/comparator.hpp`
// (docs/testing/layers.md's L1a row: "every Tier 0-5 primitive has its own
// translation unit, its own table-driven tests ... exercised directly").
// Hand-rolled `main()`, no test framework, compiled and run with a bare
// `g++ -std=c++17` by `scripts/hygiene/comparator-test.mjs` — the same
// dependency-free-TU pattern `registry_types.hpp`'s fixtures established.
//
// This is a scale-derived, absolute bound throughout — never a fixed
// epsilon (see docs/testing/comparator.md). `scripts/hygiene/forbid-test-tolerance.mjs`
// enforces that no other file under test/ defines one of its own; this
// comment is the one legitimate mention the scan's own fixture whitelists.
#include "src/kernel/comparator.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		std::fprintf(stderr, "comparator_test: FAIL — %s\n", what);
		g_failures++;
	}
}

uint64_t DoubleToBits(double v) {
	uint64_t bits;
	std::memcpy(&bits, &v, sizeof(bits));
	return bits;
}

double BitsToDouble(uint64_t bits) {
	double v;
	std::memcpy(&v, &bits, sizeof(v));
	return v;
}

// The same bit pattern comparator.hpp uses, reconstructed independently here
// rather than importing chronoduck::kStaleNaNBits — this test constructs its
// own stale-marker double the same way any fixture eventually will, instead
// of trusting the header's private constant to build the very inputs that
// exercise it.
const double kStale = BitsToDouble(0x7ff0000000000002ULL);
const double kOrdinaryNaN = std::nan("");

} // namespace

// kReorderFactor derived independently here — a second computation from the
// same primitive inputs (docs/testing/comparator.md's own shown formula),
// not merely a read of comparator.hpp's constant — so a future edit to one
// without the other (e.g. a changed sample budget with a stale factor left
// behind) is caught at compile time instead of assumed correct.
namespace derived {
constexpr double kUnitRoundoff = 1.0 / (1ull << 53);
constexpr int kMaxReorderedSamples = 4096;
constexpr double kReorderFactor = 2 * (kMaxReorderedSamples - 1) * kUnitRoundoff;
} // namespace derived

static_assert(derived::kReorderFactor == chronoduck::kReorderFactor,
              "comparator_test: kReorderFactor re-derived from docs/testing/comparator.md's formula must match "
              "comparator.hpp's own constant");

using chronoduck::equal_values;

// The NaN/stale/Inf matrix — every pairing docs/testing/comparator.md's
// branch order distinguishes, exercised directly rather than only through
// whichever fold happens to produce a NaN or an infinity first.
void TestNanStaleInfMatrix() {
	Check(DoubleToBits(kStale) == 0x7ff0000000000002ULL, "sanity: kStale really is the stale-marker bit pattern");
	Check(equal_values(1.0, 1.0, 1.0), "equal values, any scale, must match");
	Check(equal_values(0.0, -0.0, 1.0), "0.0 and -0.0 compare equal (a == b), unlike totalOrder's dedup tie-break");

	Check(equal_values(kStale, kStale, 1.0), "two stale markers are the same value");
	Check(equal_values(kOrdinaryNaN, kOrdinaryNaN, 1.0), "two ordinary NaNs are a legitimate matching answer");
	// Per the exact branch order in docs/testing/comparator.md: the second
	// check (isnan(a) && isnan(b)) fires for ANY two NaNs, stale marker
	// included, before a payload distinction could matter — so a stale
	// marker on one side and an ordinary NaN on the other still match here.
	Check(equal_values(kStale, kOrdinaryNaN, 1.0), "stale marker vs. ordinary NaN: both are NaN, so both match");
	Check(equal_values(kOrdinaryNaN, kStale, 1.0), "ordinary NaN vs. stale marker, order swapped, same result");

	Check(!equal_values(kOrdinaryNaN, 1.0, 1.0), "NaN vs. a real number must never match");
	Check(!equal_values(1.0, kOrdinaryNaN, 1.0), "a real number vs. NaN, order swapped, same result");
	Check(!equal_values(kStale, 1.0, 1.0), "stale marker vs. a real number must never match");

	double pos_inf = std::numeric_limits<double>::infinity();
	double neg_inf = -pos_inf;
	Check(equal_values(pos_inf, pos_inf, 1.0), "+Inf vs. +Inf matches");
	Check(equal_values(neg_inf, neg_inf, 1.0), "-Inf vs. -Inf matches");
	Check(!equal_values(pos_inf, neg_inf, 1.0), "+Inf vs. -Inf never matches (infinities agree only bit-identically)");
	Check(!equal_values(pos_inf, 1.0, 1.0), "+Inf vs. a finite number never matches");
	Check(!equal_values(1.0, neg_inf, 1.0), "a finite number vs. -Inf never matches");
	Check(!equal_values(pos_inf, kOrdinaryNaN, 1.0), "Inf vs. NaN never matches (the NaN branch is checked first)");
}

// One representative case per scale_kind docs/testing/comparator.md names,
// proving the single (a, b, scale) formula covers every one of them once
// the caller supplies that kind's own quantity as `scale` — the point of
// having one derivation instead of one bound per kind.
void TestScaleKinds() {
	// EXACT (scale 0): bit-exact or nothing. `docs/testing/comparator.md`:
	// "EXACT (scale 0, bit-exact) for every selection" — min, max, first,
	// last, count and the rest get no slack a nonzero scale would grant.
	Check(equal_values(5.0, 5.0, 0.0), "EXACT: identical values match at scale 0");
	Check(!equal_values(5.0, std::nextafter(5.0, 6.0), 0.0), "EXACT: even a single ULP must not match at scale 0");

	// SUM_ABS (sums and means): scale = Σ|terms|. Reproduces
	// docs/testing/comparator.md's own stated consequence verbatim — a
	// cancelling sum_over_time fixture (1e15, -1e15, 3) has scale 2e15, so a
	// naive implementation returning 0 still passes this comparator; the
	// document says the L1a cancellation table catches that class, not this
	// one, and this test exists to prove that gap is real, not assumed.
	{
		double scale = 2e15; // Σ|1e15| + Σ|-1e15| + Σ|3|, rounded to the dominant terms
		Check(equal_values(3.0, 0.0, scale),
		      "SUM_ABS: the documented cancellation gap is real at this comparator layer");
		Check(!equal_values(3.0, 100.0, scale * 1e-13),
		      "SUM_ABS: a properly small scale still rejects a real divergence");
	}

	// SUM_ABS_TIMES_FACTOR (extrapolated counters): scale = Σ|terms| times
	// the extrapolation factor, since the fold multiplies after summing.
	{
		double sum_abs = 100.0;
		double extrapolation_factor = 1.1;
		double scale = sum_abs * extrapolation_factor;
		Check(equal_values(110.0, 110.0 + chronoduck::kReorderFactor * scale * 0.5, scale),
		      "SUM_ABS_TIMES_FACTOR: drift inside the scaled bound matches");
		Check(!equal_values(110.0, 111.0, scale), "SUM_ABS_TIMES_FACTOR: a real divergence is rejected");
	}

	// RESIDUAL_SS (variance-family folds): scale = Σ(x - mean)².
	{
		double samples[] = {1.0, 2.0, 3.0, 4.0, 5.0};
		double mean = 3.0;
		double residual_ss = 0.0;
		for (double x : samples)
			residual_ss += (x - mean) * (x - mean); // (2^2+1^2+0^2+1^2+2^2) = 10
		Check(equal_values(2.5, 2.5 + chronoduck::kReorderFactor * residual_ss * 0.5, residual_ss),
		      "RESIDUAL_SS: drift inside the scaled bound matches");
		Check(!equal_values(2.5, 2.6, residual_ss), "RESIDUAL_SS: a real divergence is rejected");
	}

	// SLOPE_COND (the regression family): scale = Σ|t-t̄||v| / Σ(t-t̄)²,
	// since Σ|terms| is the wrong sum for a quotient.
	{
		double t[] = {0.0, 1.0, 2.0, 3.0};
		double v[] = {1.0, 2.0, 4.0, 8.0};
		double t_bar = 1.5;
		double numerator = 0.0, denominator = 0.0;
		for (int i = 0; i < 4; i++) {
			numerator += std::fabs(t[i] - t_bar) * std::fabs(v[i]);
			denominator += (t[i] - t_bar) * (t[i] - t_bar);
		}
		double scale = numerator / denominator;
		Check(equal_values(2.35, 2.35 + chronoduck::kReorderFactor * scale * 0.5, scale),
		      "SLOPE_COND: drift inside the scaled bound matches");
		Check(!equal_values(2.35, 2.5, scale), "SLOPE_COND: a real divergence is rejected");
	}

	// LIBM (paths whose last-bit differences come from exp/log): a fixed
	// <= 4 ULP bound per platform, per docs/testing/comparator.md. Deviation
	// (see the PR description): equal_values takes no scale_kind, so a LIBM
	// row's call site is demonstrated here by choosing `scale` so that
	// kReorderFactor * scale equals that row's declared ULP bound — the
	// same single formula, fed a different quantity, exactly like every
	// other scale_kind above.
	{
		double v = std::sqrt(2.0);
		double ulp = std::nextafter(v, v + 1.0) - v;
		double libm_bound = 4.0 * ulp;
		double scale = libm_bound / chronoduck::kReorderFactor;
		double two_ulp_off = std::nextafter(std::nextafter(v, v + 1.0), v + 1.0);
		Check(equal_values(v, two_ulp_off, scale), "LIBM: 2 ULP is within the declared 4 ULP bound");
		double far_off = v + 40.0 * ulp;
		Check(!equal_values(v, far_off, scale), "LIBM: 40 ULP is well outside the declared 4 ULP bound");
	}
}

// The comparator headroom pin (docs/testing/layers.md's L13 row): records
// the largest drift ever accepted and the smallest real divergence ever
// rejected, and fails if the derivation has moved toward either since.
void TestHeadroomPin() {
	auto margins = chronoduck::ReorderFactorHeadroom(chronoduck::kReorderFactor, chronoduck::kUnitRoundoff);
	// docs/testing/comparator.md's own stated floors — "three to four
	// orders" on the accept side, "ten orders" on the reject side.
	Check(margins.accept_orders >= 1e3, "headroom: accept-side margin has eroded below the documented three orders");
	Check(margins.reject_orders >= 1e10, "headroom: reject-side margin has eroded below the documented ten orders");

	// And the two real reference points the margins above are measured
	// against must still resolve the way they always have, not just clear
	// the margin arithmetic in the abstract.
	double scale = 1.0;
	double largest_accepted_drift = 5.0 * chronoduck::kUnitRoundoff * scale; // "1-5 ULP"
	Check(equal_values(scale, scale + largest_accepted_drift, scale),
	      "headroom: the largest historically-accepted drift (5 ULP) must still be accepted");
	double smallest_rejected_divergence = 3e-2 * scale; // the duplicate-timestamp bug
	Check(!equal_values(scale, scale + smallest_rejected_divergence, scale),
	      "headroom: the smallest historically-rejected divergence (3e-2) must still be rejected");
}

int main() {
	TestNanStaleInfMatrix();
	TestScaleKinds();
	TestHeadroomPin();

	if (g_failures > 0) {
		std::fprintf(stderr, "comparator_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("comparator_test: PASS\n");
	return 0;
}
