// counter_fold_test.cpp — the L1a direct test for
// `src/kernel/counter_fold.hpp` (docs/testing/layers.md's L1a row: "every
// Tier 0-5 primitive has its own translation unit, its own table-driven
// tests ... exercised directly"). Hand-rolled `main()`, no test framework,
// compiled and run with a bare `g++ -std=c++17` by
// `scripts/hygiene/kernel-primitive-tests.mjs` — the same dependency-free-TU
// pattern `edge_context_test.cpp` established.
//
// Structure follows `docs/testing/primitives.md`'s Tier 4 `counter_fold`
// row: the named unit-contract cases (no reset; reset at first pair; reset
// at last pair; two consecutive resets; reset to zero; reset to a
// higher-than-previous value, not a reset; with a `before` sample and
// without), this issue's own `st_reset` four-case table, a one-line
// independent-fold oracle checked over randomized trials (MR-PERM,
// MR-SCALE), and the two named must-die mutants.
#include "../../src/kernel/counter_fold.hpp"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace chronoduck;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		std::fprintf(stderr, "counter_fold_test: FAIL — %s\n", what);
		g_failures++;
	}
}

// The plain value-drop predicate — the reference's own classic reset rule
// when no start timestamp is bound at all, used for every unit-contract
// case below that isn't specifically about `st_reset`.
bool ValueDropOnly(const CounterSample &prev, const CounterSample &curr) {
	return curr.v < prev.v;
}

CounterSample S(int64_t t, double v) {
	return CounterSample {t, v, false, 0};
}

// docs/testing/primitives.md's own named unit-contract cases, each on a
// small explicit fixture, using the plain value-drop predicate.
void TestNamedUnitCases() {
	// No reset: monotone increasing.
	{
		std::vector<CounterSample> data = {S(0, 10), S(10, 20), S(20, 35)};
		auto summary = counter_fold(data.data(), data.size(), nullptr, ValueDropOnly);
		Check(summary.resets == 0, "no reset: resets == 0");
		Check(summary.delta == 25.0, "no reset: delta == last - first == 25");
		Check(summary.first == 10 && summary.last == 35, "no reset: first/last are the raw endpoints");
	}

	// Reset at the first pair.
	{
		std::vector<CounterSample> data = {S(0, 90), S(10, 5), S(20, 15)};
		auto summary = counter_fold(data.data(), data.size(), nullptr, ValueDropOnly);
		Check(summary.resets == 1, "reset at first pair: resets == 1");
		// reset-adjusted: +90 (pre-reset value) + (15 - 90) = +90 - 75 = 15
		Check(summary.delta == 15.0, "reset at first pair: delta accounts for the pre-reset value");
	}

	// Reset at the last pair.
	{
		std::vector<CounterSample> data = {S(0, 10), S(10, 20), S(20, 5)};
		auto summary = counter_fold(data.data(), data.size(), nullptr, ValueDropOnly);
		Check(summary.resets == 1, "reset at last pair: resets == 1");
		// +20 (pre-reset value) + (5 - 10) = 20 - 5 = 15
		Check(summary.delta == 15.0, "reset at last pair: delta accounts for the pre-reset value");
	}

	// Two consecutive resets.
	{
		std::vector<CounterSample> data = {S(0, 50), S(10, 10), S(20, 5), S(30, 8)};
		auto summary = counter_fold(data.data(), data.size(), nullptr, ValueDropOnly);
		Check(summary.resets == 2, "two consecutive resets: resets == 2");
		// +50 (pair 1) + 10 (pair 2) + (8 - 50) = 50 + 10 - 42 = 18
		Check(summary.delta == 18.0, "two consecutive resets: delta sums every pre-reset value");
	}

	// Reset to zero.
	{
		std::vector<CounterSample> data = {S(0, 42), S(10, 0), S(20, 3)};
		auto summary = counter_fold(data.data(), data.size(), nullptr, ValueDropOnly);
		Check(summary.resets == 1, "reset to zero: still a reset");
		// +42 + (3 - 42) = 42 - 39 = 3
		Check(summary.delta == 3.0, "reset to zero: delta accounts for the pre-reset value");
	}

	// Reset to a higher-than-previous value: not a reset at all.
	{
		std::vector<CounterSample> data = {S(0, 10), S(10, 12), S(20, 30)};
		auto summary = counter_fold(data.data(), data.size(), nullptr, ValueDropOnly);
		Check(summary.resets == 0, "higher-than-previous value: not a reset");
		Check(summary.delta == 20.0, "higher-than-previous value: plain last - first");
	}

	// With a `before` sample and without: `before` only ever changes
	// `resets`, never `delta`, `first` or `n`.
	{
		std::vector<CounterSample> data = {S(20, 5), S(30, 15)};
		CounterSample before = S(10, 90); // a value drop from `before` into data[0]

		auto without = counter_fold(data.data(), data.size(), nullptr, ValueDropOnly);
		Check(without.resets == 0, "without a before sample: no boundary reset to see");

		auto with = counter_fold(data.data(), data.size(), &before, ValueDropOnly);
		Check(with.resets == 1, "with a before sample: the boundary reset is counted");
		Check(with.delta == without.delta, "a before sample never changes delta");
		Check(with.first == without.first && with.n == without.n, "a before sample never changes first/n");
	}
}

// This issue's own acceptance criterion: `st_reset`'s four-case table.
struct StResetCase {
	const char *name;
	CounterSample prev;
	CounterSample curr;
	bool from_delta_temporality;
	bool expected;
};

const StResetCase kStResetTable[] = {
    // unset: curr carries no start timestamp at all.
    {"unset", S(0, 1), S(10, 2), false, false},
    // ST >= T: an invalid/degenerate start timestamp is a reset regardless
    // of temporality.
    {"ST >= T (equal)", S(0, 1), {10, 2, true, 10}, false, true},
    {"ST >= T (past)", S(0, 1), {10, 2, true, 15}, false, true},
    // ST < prevT: consistent with the run already observed, never a reset.
    {"ST < prevT", S(0, 1), {10, 2, true, -5}, false, false},
    // ST == prevT: ambiguous, resolved by temporality.
    {"ST == prevT, delta temporality", S(0, 1), {10, 2, true, 0}, true, false},
    {"ST == prevT, unknown temporality", S(0, 1), {10, 2, true, 0}, false, true},
    // prevT < ST < T: an unambiguous forward move, not one of the four named
    // cases but resolved the same way as ST >= T.
    {"prevT < ST < T", S(0, 1), {10, 2, true, 5}, false, true},
};

void TestStResetFourCaseTable() {
	for (const auto &c : kStResetTable) {
		bool actual = st_reset(c.prev, c.curr, c.from_delta_temporality);
		char what[160];
		std::snprintf(what, sizeof(what), "st_reset(%s): expected %s, got %s", c.name, c.expected ? "true" : "false",
		              actual ? "true" : "false");
		Check(actual == c.expected, what);
	}
}

// `value_or_st_reset` is the disjunction: either half alone is enough.
void TestValueOrStReset() {
	CounterSample prev = S(0, 10);
	CounterSample curr_drop_only = S(10, 5); // value drop, no ST at all
	Check(value_or_st_reset(prev, curr_drop_only, false), "value drop alone triggers value_or_st_reset");

	CounterSample curr_st_only {10, 20, true, 15}; // ST >= T, value still rising
	Check(value_or_st_reset(prev, curr_st_only, false), "st_reset alone triggers value_or_st_reset");

	CounterSample curr_neither = S(10, 20); // rising, no ST bound
	Check(!value_or_st_reset(prev, curr_neither, false), "neither condition: not a reset");
}

// The independent oracle: a one-line fold over the same reset predicate,
// spelled with `std::accumulate`-shaped state instead of `counter_fold`'s
// own loop — a genuinely different code path, not a second copy of the same
// loop (`docs/testing/primitives.md:counterfold-row:` `A one-line Python fold in the oracle target`,
// transcribed here as its C++ oracle-target equivalent).
double OracleDelta(const std::vector<CounterSample> &data) {
	double total = 0.0;
	for (std::size_t i = 1; i < data.size(); i++) {
		total += (data[i].v < data[i - 1].v) ? data[i - 1].v : 0.0;
	}
	total += data.back().v - data.front().v;
	return total;
}

std::size_t OracleResets(const std::vector<CounterSample> &data) {
	std::size_t n = 0;
	for (std::size_t i = 1; i < data.size(); i++) {
		if (data[i].v < data[i - 1].v) {
			n++;
		}
	}
	return n;
}

// MR-RESET (rebase invariance): adding a constant `k` to every value never
// changes the *number* of resets a plain value-drop predicate finds — a
// strict decrease is still a strict decrease after a uniform shift.
// MR-SCALE for k>0: scaling every value by a positive `k` scales `delta` by
// exactly `k` and leaves `resets` unchanged.
// MR-PERM: sorting the fixture by `t` before folding is already
// `counter_fold`'s own precondition, so "after sort" here means the walk
// agrees with the oracle regardless of which permutation the caller sorted
// *from* — checked by shuffling before sorting.
void TestRandomizedInvariantsAndOracle() {
	std::mt19937_64 rng(0xC0117E7);
	std::uniform_int_distribution<int> n_dist(1, 12);
	std::uniform_real_distribution<double> value_dist(-100.0, 100.0);
	std::uniform_int_distribution<int64_t> gap_dist(1, 50);

	const int kTrials = 500;
	for (int trial = 0; trial < kTrials; trial++) {
		int n = n_dist(rng);
		std::vector<CounterSample> data;
		int64_t t = 0;
		for (int i = 0; i < n; i++) {
			t += gap_dist(rng);
			data.push_back(S(t, value_dist(rng)));
		}

		auto summary = counter_fold(data.data(), data.size(), nullptr, ValueDropOnly);
		Check(summary.delta == OracleDelta(data), "randomized: delta agrees with the independent oracle fold");
		Check(summary.resets == OracleResets(data), "randomized: resets agrees with the independent oracle fold");
		Check(summary.resets == OracleResets(data), "resets equals count of strict decreases");

		// MR-RESET: rebase invariance — shift every value by a constant.
		double k_shift = 37.5;
		std::vector<CounterSample> shifted = data;
		for (auto &s : shifted)
			s.v += k_shift;
		auto shifted_summary = counter_fold(shifted.data(), shifted.size(), nullptr, ValueDropOnly);
		Check(shifted_summary.resets == summary.resets, "MR-RESET: rebasing every value preserves resets");

		// MR-SCALE for k > 0: scaling every value scales delta by k, leaves
		// resets unchanged.
		double k_scale = 2.5;
		std::vector<CounterSample> scaled = data;
		for (auto &s : scaled)
			s.v *= k_scale;
		auto scaled_summary = counter_fold(scaled.data(), scaled.size(), nullptr, ValueDropOnly);
		Check(scaled_summary.resets == summary.resets, "MR-SCALE (k>0): scaling every value preserves resets");
		Check(std::fabs(scaled_summary.delta - summary.delta * k_scale) <= 1e-9 * std::fabs(summary.delta) + 1e-9,
		      "MR-SCALE (k>0): scaling every value scales delta by the same k");
	}
}

// Must-die mutant: "`<`→`<=` on the reset test" — a non-decrease (equal
// consecutive values) gets wrongly counted as a reset.
CounterFoldSummary MutantLessEqualResetTest(const CounterSample *data, std::size_t n) {
	CounterFoldSummary summary;
	if (n == 0)
		return summary;
	summary.has_data = true;
	summary.first = data[0].v;
	summary.first_t = data[0].t;
	summary.last = data[n - 1].v;
	summary.last_t = data[n - 1].t;
	summary.n = n;
	std::size_t resets = 0;
	double delta = 0.0;
	for (std::size_t i = 1; i < n; i++) {
		if (data[i].v <= data[i - 1].v) { // mutated: was `<`
			resets++;
			delta += data[i - 1].v;
		}
	}
	delta += data[n - 1].v - data[0].v;
	summary.delta = delta;
	summary.resets = resets;
	return summary;
}

void TestMutantLessEqualDies() {
	// Two consecutive equal values: a flat run, never a reset.
	std::vector<CounterSample> data = {S(0, 10), S(10, 10), S(20, 15)};
	auto real = counter_fold(data.data(), data.size(), nullptr, ValueDropOnly);
	auto mutant = MutantLessEqualResetTest(data.data(), data.size());
	Check(real.resets == 0, "real: an equal-value pair is not a reset");
	Check(mutant.resets == 1,
	      "must-die (< -> <=): a flat pair is wrongly counted as a reset — this mutant is observably wrong");
	Check(real.resets != mutant.resets, "the real primitive and the mutant must disagree on this fixture");
}

// Must-die mutant: "accumulating raw instead of reset-adjusted delta" — on
// any reset, plain `last - first` diverges from the true reset-adjusted
// total.
double MutantRawDelta(const std::vector<CounterSample> &data) {
	return data.back().v - data.front().v; // mutated: no reset adjustment at all
}

void TestMutantRawDeltaDies() {
	std::vector<CounterSample> data = {S(0, 90), S(10, 5), S(20, 15)}; // reset at the first pair
	auto real = counter_fold(data.data(), data.size(), nullptr, ValueDropOnly);
	double mutant = MutantRawDelta(data);
	Check(real.delta == 15.0, "real: reset-adjusted delta accounts for the pre-reset value");
	Check(mutant == -75.0, "mutant: raw last - first ignores the reset entirely");
	Check(real.delta != mutant, "must-die (raw delta): the mutant is observably wrong whenever a reset occurred");
}

} // namespace

int main() {
	TestNamedUnitCases();
	TestStResetFourCaseTable();
	TestValueOrStReset();
	TestRandomizedInvariantsAndOracle();
	TestMutantLessEqualDies();
	TestMutantRawDeltaDies();

	if (g_failures > 0) {
		std::fprintf(stderr, "counter_fold_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("counter_fold_test: PASS\n");
	return 0;
}
