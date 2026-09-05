// extrapolate_test.cpp — the L1a direct test for
// `src/kernel/extrapolate.hpp` (docs/testing/layers.md's L1a row: "every
// Tier 0-5 primitive has its own translation unit, its own table-driven
// tests ... exercised directly"). Hand-rolled `main()`, no test framework,
// compiled and run with a bare `g++ -std=c++17` by
// `scripts/hygiene/kernel-primitive-tests.mjs` — the same dependency-free-TU
// pattern `counter_fold_test.cpp` established.
//
// Structure follows `docs/testing/primitives.md`'s Tier 4 `extrapolate`
// row, `EXTRAPOLATE` only per this issue's own stated scope: every branch as
// its own row (gap under/over threshold on each side, the `durationToZero`
// clamp engaged and not, two samples exactly on the edges, n = 2 minimal,
// "not enough samples" → no value, plus this issue's own single-sample
// with a bound start timestamp), the `factor >= 1` invariant, and the five
// named must-die mutants.
#include "src/kernel/comparator.hpp"
#include "src/kernel/counter_fold.hpp"
#include "src/kernel/extrapolate.hpp"

#include <cmath>
#include <cstdio>

using namespace chronoduck;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		std::fprintf(stderr, "extrapolate_test: FAIL — %s\n", what);
		g_failures++;
	}
}

// A summary built directly (bypassing `counter_fold`) so every row below
// states exactly the fold outcome it means to test, independent of whether
// `counter_fold` itself is correct — this file's whole job is `extrapolate`
// alone, "as a pure function of the fold summary".
CounterFoldSummary MakeSummary(double first, int64_t first_t, double last, int64_t last_t, std::size_t n,
                               double delta) {
	CounterFoldSummary s;
	s.has_data = true;
	s.first = first;
	s.first_t = first_t;
	s.last = last;
	s.last_t = last_t;
	s.n = n;
	s.delta = delta;
	return s;
}

// "not enough samples" → no value, every mode alike: zero and one sample
// (no bound start timestamp) both yield nothing.
void TestNotEnoughSamples() {
	CounterFoldSummary empty;
	Check(!extrapolate(empty, 0, 100, true).has_value, "zero samples: no value");

	CounterFoldSummary one = MakeSummary(5.0, 10, 5.0, 10, 1, 0.0);
	Check(!extrapolate(one, 0, 100, true).has_value, "one sample, no start timestamp: no value");
}

// Two samples exactly on the window edges: sampled_interval == the window
// width, both duration_to_* terms are 0, so factor == 1 exactly (the
// invariant `docs/testing/primitives.md:extrapolate-row:` `factor ≥ 1 for EXTRAPOLATE`
// at its tightest case).
void TestTwoSamplesExactlyOnEdges() {
	CounterFoldSummary s = MakeSummary(10.0, 0, 30.0, 100, 2, 20.0);
	auto r = extrapolate(s, 0, 100, true);
	Check(r.has_value, "two samples on the edges: has a value");
	Check(r.factor == 1.0, "two samples on the edges: factor == 1 exactly (n=2 minimal, no gap either side)");
	Check(r.value == 20.0, "two samples on the edges: value == the plain delta when factor == 1");
}

// n = 2 minimal, with genuine gaps on both sides under the threshold: the
// classic case, cross-checked against the reference's own arithmetic
// transcribed independently here (the oracle target the design doc names —
// `docs/testing/primitives.md:extrapolate-row:` `transcribed into the oracle target as pure arithmetic`).
double OracleExtrapolatedDelta(double first, int64_t first_t, double /*last*/, int64_t last_t, std::size_t n,
                               double raw_delta, int64_t window_start, int64_t window_end, bool is_counter) {
	double sampled_interval = static_cast<double>(last_t - first_t);
	double avg_interval = sampled_interval / static_cast<double>(n - 1);
	double threshold = avg_interval * 1.1;

	double duration_to_start = static_cast<double>(first_t - window_start);
	if (duration_to_start > threshold)
		duration_to_start = avg_interval / 2.0;

	if (is_counter && raw_delta > 0.0 && first >= 0.0) {
		double duration_to_zero = sampled_interval * (first / raw_delta);
		if (duration_to_zero < duration_to_start)
			duration_to_start = duration_to_zero;
	}

	double duration_to_end = static_cast<double>(window_end - last_t);
	if (duration_to_end > threshold)
		duration_to_end = avg_interval / 2.0;

	double extrapolate_to_interval = sampled_interval + duration_to_start + duration_to_end;
	return raw_delta * (extrapolate_to_interval / sampled_interval);
}

void TestGapUnderThresholdBothSides() {
	// Samples at 100, 110, 120 (avg interval 10, threshold 11); window
	// [95, 125]: duration_to_start = 5 < 11, duration_to_end = 5 < 11 — both
	// sides extrapolate fully to the boundary.
	CounterFoldSummary s = MakeSummary(1.0, 100, 3.0, 120, 3, 2.0);
	auto r = extrapolate(s, 95, 125, true);
	double oracle = OracleExtrapolatedDelta(1.0, 100, 3.0, 120, 3, 2.0, 95, 125, true);
	Check(r.has_value, "gap under threshold both sides: has a value");
	Check(equal_values(r.value, oracle, std::fabs(2.0) * r.factor),
	      "gap under threshold both sides: matches the independent oracle within SUM_ABS_TIMES_FACTOR scale");
	Check(r.factor > 1.0, "gap under threshold both sides: factor > 1 (both edges extended)");
}

void TestGapOverThresholdLeftOnlyRightOnlyBoth() {
	// avg interval 10, threshold 11. Left gap 50 (over), right gap 5 (under).
	{
		CounterFoldSummary s = MakeSummary(1.0, 100, 3.0, 120, 3, 2.0);
		auto r = extrapolate(s, 50, 125, true);
		double oracle = OracleExtrapolatedDelta(1.0, 100, 3.0, 120, 3, 2.0, 50, 125, true);
		Check(equal_values(r.value, oracle, std::fabs(2.0) * r.factor), "left only over threshold: matches oracle");
	}
	// Right gap 50 (over), left gap 5 (under).
	{
		CounterFoldSummary s = MakeSummary(1.0, 100, 3.0, 120, 3, 2.0);
		auto r = extrapolate(s, 95, 170, true);
		double oracle = OracleExtrapolatedDelta(1.0, 100, 3.0, 120, 3, 2.0, 95, 170, true);
		Check(equal_values(r.value, oracle, std::fabs(2.0) * r.factor), "right only over threshold: matches oracle");
	}
	// Both over threshold.
	{
		CounterFoldSummary s = MakeSummary(1.0, 100, 3.0, 120, 3, 2.0);
		auto r = extrapolate(s, 0, 300, true);
		double oracle = OracleExtrapolatedDelta(1.0, 100, 3.0, 120, 3, 2.0, 0, 300, true);
		Check(equal_values(r.value, oracle, std::fabs(2.0) * r.factor), "both over threshold: matches oracle");
	}
}

// "Threshold just under/over": duration_to_start on either side of the
// exact 1.1x threshold value, demonstrating the two branches directly (not
// only via the mutant test below).
void TestThresholdJustUnderOver() {
	// Ticks are integral, so a clean split around the (also-integral, for
	// spacing 10) threshold of 11 needs a scaled-up sample spacing instead —
	// avg_interval = 100 (samples at 1000, 1100, 1200), threshold = 110.
	// `is_counter = false` throughout: this row is about the threshold branch
	// alone, isolated from the zero clamp (its own row below), which would
	// otherwise override whichever duration_to_start this test means to
	// observe.
	CounterFoldSummary scaled = MakeSummary(1.0, 1000, 3.0, 1200, 3, 2.0); // avg 100, threshold 110
	{
		auto r = extrapolate(scaled, 1000 - 109, 1200 + 5, false); // duration_to_start = 109 < 110
		double expected_extra = 109.0;                             // extrapolates fully, no half-interval fallback
		double sampled_interval = 200.0;
		double duration_to_end = 5.0; // < 110, extrapolates fully too
		double factor = (sampled_interval + expected_extra + duration_to_end) / sampled_interval;
		Check(equal_values(r.factor, factor, 1e-9), "threshold just under: extrapolates fully to the boundary");
	}
	// Just over: duration_to_start = 111 > 110 -> falls back to avg/2 = 50.
	{
		auto r = extrapolate(scaled, 1000 - 111, 1200 + 5, false);
		double sampled_interval = 200.0;
		double duration_to_start = 100.0 / 2.0; // half-interval fallback
		double duration_to_end = 5.0;
		double factor = (sampled_interval + duration_to_start + duration_to_end) / sampled_interval;
		Check(equal_values(r.factor, factor, 1e-9), "threshold just over: falls back to the half interval");
	}
	// Exactly at the threshold: still extrapolates fully (strict `>`, not
	// `>=`).
	{
		auto r = extrapolate(scaled, 1000 - 110, 1200 + 5, false);
		double sampled_interval = 200.0;
		double duration_to_start = 110.0; // exactly at threshold: full extrapolation, per strict `>`
		double duration_to_end = 5.0;
		double factor = (sampled_interval + duration_to_start + duration_to_end) / sampled_interval;
		Check(equal_values(r.factor, factor, 1e-9), "threshold exactly at the boundary: still extrapolates fully");
	}
}

// "Clamp engaged/not": the `durationToZero` zero clamp for `COUNTER` inputs.
void TestClampEngagedAndNot() {
	// Engaged: first value small relative to delta, so durationToZero is
	// smaller than the (large, over-threshold-fallback) duration_to_start it
	// would otherwise get.
	{
		// avg_interval = 10, threshold = 11. first = 1, delta = 100 (last =
		// 101). durationToZero = sampled_interval * (first/delta) = 20 * 0.01
		// = 0.2, far smaller than the half-interval fallback (5) the
		// over-threshold left gap would otherwise select.
		CounterFoldSummary s = MakeSummary(1.0, 100, 101.0, 120, 3, 100.0);
		auto r = extrapolate(s, 0, 125, true); // left gap 100, far over threshold 11
		double sampled_interval = 20.0;
		double duration_to_zero = sampled_interval * (1.0 / 100.0);
		double duration_to_end = 5.0; // under threshold, extrapolates fully
		double factor = (sampled_interval + duration_to_zero + duration_to_end) / sampled_interval;
		Check(equal_values(r.factor, factor, 1e-9), "clamp engaged: durationToZero wins over the fallback");
	}
	// Not engaged: not a COUNTER domain, so the clamp never applies even
	// though the same shape would engage it above.
	{
		CounterFoldSummary s = MakeSummary(1.0, 100, 101.0, 120, 3, 100.0);
		auto r_counter = extrapolate(s, 0, 125, true);
		auto r_gauge = extrapolate(s, 0, 125, false);
		Check(r_gauge.factor != r_counter.factor, "clamp not engaged for a non-COUNTER domain: differs from COUNTER");
	}
	// Not engaged: delta <= 0 disables the clamp even for a COUNTER.
	{
		CounterFoldSummary s = MakeSummary(1.0, 100, 0.5, 120, 3, -0.5);
		auto r = extrapolate(s, 0, 125, true);
		// duration_to_start should fall back to half-interval (5), not any
		// zero-clamped value, since eff_delta <= 0 disables the clamp check.
		double sampled_interval = 20.0;
		double duration_to_start = 5.0; // half-interval fallback, left gap 100 is over threshold 11
		double duration_to_end = 5.0;
		double factor = (sampled_interval + duration_to_start + duration_to_end) / sampled_interval;
		Check(equal_values(r.factor, factor, 1e-9), "clamp not engaged: non-positive delta leaves the fallback alone");
	}
}

// This issue's own single-sample case: a start timestamp strictly inside
// the left gap lets one sample yield a rate.
void TestSingleSampleWithStartTimestamp() {
	CounterFoldSummary s;
	s.has_data = true;
	s.first = 5.0;
	s.first_t = 100;
	s.first_has_st = true;
	s.first_st = 80; // strictly inside (window_start=50, first_t=100)
	s.last = 5.0;
	s.last_t = 100;
	s.n = 1;
	s.delta = 0.0; // counter_fold's own answer for n=1: no adjacent pair to fold

	auto r = extrapolate(s, 50, 110, true);
	Check(r.has_value, "single sample with a bound, in-window start timestamp: yields a value");

	// Synthetic point (80, 0.0) plus the real (100, 5.0): sampled_interval =
	// 20, avg_interval = 20 (eff_n=2), threshold = 22. duration_to_start = 0
	// (the short-circuit). duration_to_end = 110 - 100 = 10 < 22: extrapolates
	// fully.
	double sampled_interval = 20.0;
	double duration_to_end = 10.0;
	double factor = (sampled_interval + 0.0 + duration_to_end) / sampled_interval;
	double eff_delta = 5.0; // 0.0 (synthetic delta contribution) + summary.first
	Check(equal_values(r.factor, factor, 1e-9), "single sample with ST: factor matches the synthetic-point arithmetic");
	Check(equal_values(r.value, eff_delta * factor, 1e-9 * std::fabs(eff_delta * factor)),
	      "single sample with ST: value is the first value counted, extrapolated");

	// ST at or before window_start: no short-circuit, falls through to "not
	// enough samples" (n=1, no second real point to form a slope).
	CounterFoldSummary s2 = s;
	s2.first_st = 50; // == window_start, not strictly inside
	Check(!extrapolate(s2, 50, 110, true).has_value, "ST at window_start (not strictly inside): no short-circuit");

	// ST >= first_t: degenerate, also no short-circuit.
	CounterFoldSummary s3 = s;
	s3.first_st = 100;
	Check(!extrapolate(s3, 50, 110, true).has_value, "ST >= first_t: no short-circuit, still not enough samples");

	// Not a COUNTER domain: the short-circuit never applies.
	Check(!extrapolate(s, 50, 110, false).has_value, "GAUGE domain: the start-timestamp short-circuit never applies");
}

// factor >= 1 for EXTRAPOLATE, swept over many window/sample combinations
// (`docs/testing/primitives.md:extrapolate-row:` `factor ≥ 1 for EXTRAPOLATE`):
// extrapolation only ever adds duration on top of the sampled interval, it
// never subtracts.
void TestFactorAlwaysAtLeastOne() {
	const int64_t firsts[] = {0, 10, 100};
	const int64_t spans[] = {1, 5, 20, 100};
	const int64_t left_gaps[] = {0, 1, 5, 50, 1000};
	const int64_t right_gaps[] = {0, 1, 5, 50, 1000};
	for (int64_t first_t : firsts) {
		for (int64_t span : spans) {
			int64_t last_t = first_t + span;
			for (int64_t left_gap : left_gaps) {
				for (int64_t right_gap : right_gaps) {
					CounterFoldSummary s = MakeSummary(10.0, first_t, 25.0, last_t, 2, 15.0);
					auto r = extrapolate(s, first_t - left_gap, last_t + right_gap, true);
					Check(r.has_value, "factor sweep: n=2 always has a value");
					Check(r.factor >= 1.0, "factor sweep: factor is always >= 1 for EXTRAPOLATE");
				}
			}
		}
	}
}

// Must-die mutant: "1.1 -> any other constant".
double MutantThresholdConstant(double summary_first, int64_t summary_first_t, double summary_last,
                               int64_t summary_last_t, std::size_t n, double delta, int64_t window_start,
                               int64_t window_end, double mutant_constant) {
	double sampled_interval = static_cast<double>(summary_last_t - summary_first_t);
	double avg_interval = sampled_interval / static_cast<double>(n - 1);
	double threshold = avg_interval * mutant_constant; // mutated: was 1.1
	double duration_to_start = static_cast<double>(summary_first_t - window_start);
	if (duration_to_start > threshold)
		duration_to_start = avg_interval / 2.0;
	if (delta > 0.0 && summary_first >= 0.0) {
		double duration_to_zero = sampled_interval * (summary_first / delta);
		if (duration_to_zero < duration_to_start)
			duration_to_start = duration_to_zero;
	}
	double duration_to_end = static_cast<double>(window_end - summary_last_t);
	if (duration_to_end > threshold)
		duration_to_end = avg_interval / 2.0;
	double extrapolate_to_interval = sampled_interval + duration_to_start + duration_to_end;
	(void)summary_last;
	return delta * (extrapolate_to_interval / sampled_interval);
}

void TestMutantThresholdConstantDies() {
	// avg_interval = 10, real threshold = 11. Left gap 10.5 straddles the
	// real threshold but not a mutant threshold of, say, 1.0 (=10).
	CounterFoldSummary s = MakeSummary(1.0, 100, 3.0, 120, 3, 2.0);
	int64_t window_start = 100 - 11; // integral ticks: gap 11, > mutant threshold 10, <= real threshold 11
	auto real = extrapolate(s, window_start, 120 + 5, true);
	double mutant = MutantThresholdConstant(1.0, 100, 3.0, 120, 3, 2.0, window_start, 120 + 5, 1.0);
	Check(real.value != mutant,
	      "must-die (1.1 -> other constant): a gap straddling the real threshold but not the mutant's must diverge");
}

// Must-die mutant: "/2 removed" — the half-interval fallback uses the bare
// average interval instead of half of it.
double MutantHalfIntervalRemoved(double first, int64_t first_t, double last, int64_t last_t, std::size_t n,
                                 double delta, int64_t window_start, int64_t window_end) {
	(void)last;
	double sampled_interval = static_cast<double>(last_t - first_t);
	double avg_interval = sampled_interval / static_cast<double>(n - 1);
	double threshold = avg_interval * 1.1;
	double duration_to_start = static_cast<double>(first_t - window_start);
	if (duration_to_start > threshold)
		duration_to_start = avg_interval; // mutated: was avg_interval / 2.0
	if (delta > 0.0 && first >= 0.0) {
		double duration_to_zero = sampled_interval * (first / delta);
		if (duration_to_zero < duration_to_start)
			duration_to_start = duration_to_zero;
	}
	double duration_to_end = static_cast<double>(window_end - last_t);
	if (duration_to_end > threshold)
		duration_to_end = avg_interval; // mutated
	double extrapolate_to_interval = sampled_interval + duration_to_start + duration_to_end;
	return delta * (extrapolate_to_interval / sampled_interval);
}

void TestMutantHalfIntervalRemovedDies() {
	// Both sides far over threshold, so both fall back to the (mutated,
	// doubled) interval.
	CounterFoldSummary s = MakeSummary(1.0, 100, 3.0, 120, 3, 2.0);
	auto real = extrapolate(s, 0, 300, true);
	double mutant = MutantHalfIntervalRemoved(1.0, 100, 3.0, 120, 3, 2.0, 0, 300);
	Check(real.value != mutant, "must-die (/2 removed): the fallback must use half the average interval");
}

// Must-die mutant: "clamp direction" — the zero clamp engages when
// durationToZero is *larger* than duration_to_start instead of smaller,
// extending the extrapolation instead of shrinking it.
double MutantClampDirectionFlipped(double first, int64_t first_t, double last, int64_t last_t, std::size_t n,
                                   double delta, int64_t window_start, int64_t window_end) {
	(void)last;
	double sampled_interval = static_cast<double>(last_t - first_t);
	double avg_interval = sampled_interval / static_cast<double>(n - 1);
	double threshold = avg_interval * 1.1;
	double duration_to_start = static_cast<double>(first_t - window_start);
	if (duration_to_start > threshold)
		duration_to_start = avg_interval / 2.0;
	if (delta > 0.0 && first >= 0.0) {
		double duration_to_zero = sampled_interval * (first / delta);
		if (duration_to_zero > duration_to_start) { // mutated: was `<`
			duration_to_start = duration_to_zero;
		}
	}
	double duration_to_end = static_cast<double>(window_end - last_t);
	if (duration_to_end > threshold)
		duration_to_end = avg_interval / 2.0;
	double extrapolate_to_interval = sampled_interval + duration_to_start + duration_to_end;
	return delta * (extrapolate_to_interval / sampled_interval);
}

void TestMutantClampDirectionDies() {
	// Same fixture as the "clamp engaged" test: durationToZero (0.2) is much
	// smaller than the fallback it should replace (5). The mutant, with the
	// comparison flipped, refuses to engage here (0.2 > 5 is false) and
	// keeps the fallback instead — real and mutant diverge.
	CounterFoldSummary s = MakeSummary(1.0, 100, 101.0, 120, 3, 100.0);
	auto real = extrapolate(s, 0, 125, true);
	double mutant = MutantClampDirectionFlipped(1.0, 100, 101.0, 120, 3, 100.0, 0, 125);
	Check(real.value != mutant, "must-die (clamp direction): the clamp must only ever shrink duration_to_start");
}

// Must-die mutant: "n-1 -> n in the average interval".
double MutantAverageIntervalUsesN(double first, int64_t first_t, double last, int64_t last_t, std::size_t n,
                                  double delta, int64_t window_start, int64_t window_end) {
	(void)last;
	double sampled_interval = static_cast<double>(last_t - first_t);
	double avg_interval = sampled_interval / static_cast<double>(n); // mutated: was (n - 1)
	double threshold = avg_interval * 1.1;
	double duration_to_start = static_cast<double>(first_t - window_start);
	if (duration_to_start > threshold)
		duration_to_start = avg_interval / 2.0;
	if (delta > 0.0 && first >= 0.0) {
		double duration_to_zero = sampled_interval * (first / delta);
		if (duration_to_zero < duration_to_start)
			duration_to_start = duration_to_zero;
	}
	double duration_to_end = static_cast<double>(window_end - last_t);
	if (duration_to_end > threshold)
		duration_to_end = avg_interval / 2.0;
	double extrapolate_to_interval = sampled_interval + duration_to_start + duration_to_end;
	return delta * (extrapolate_to_interval / sampled_interval);
}

void TestMutantAverageIntervalUsesNDies() {
	// 3 samples spanning 20 ticks: real avg_interval = 10 (n-1=2), mutant
	// avg_interval = 6.667 (n=3) — different enough to select different
	// threshold branches for a gap of 10.
	CounterFoldSummary s = MakeSummary(1.0, 100, 3.0, 120, 3, 2.0);
	int64_t window_start = 100 - 10; // gap 10: under real threshold 11, over mutant threshold 7.33
	auto real = extrapolate(s, window_start, 120 + 1, true);
	double mutant = MutantAverageIntervalUsesN(1.0, 100, 3.0, 120, 3, 2.0, window_start, 120 + 1);
	Check(real.value != mutant, "must-die (n-1 -> n): a mis-sized average interval selects the wrong branch");
}

} // namespace

int main() {
	TestNotEnoughSamples();
	TestTwoSamplesExactlyOnEdges();
	TestGapUnderThresholdBothSides();
	TestGapOverThresholdLeftOnlyRightOnlyBoth();
	TestThresholdJustUnderOver();
	TestClampEngagedAndNot();
	TestSingleSampleWithStartTimestamp();
	TestFactorAlwaysAtLeastOne();
	TestMutantThresholdConstantDies();
	TestMutantHalfIntervalRemovedDies();
	TestMutantClampDirectionDies();
	TestMutantAverageIntervalUsesNDies();

	if (g_failures > 0) {
		std::fprintf(stderr, "extrapolate_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("extrapolate_test: PASS\n");
	return 0;
}
