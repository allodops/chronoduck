// lookback_test.cpp — the L1a direct test for `src/kernel/lookback.hpp`
// (docs/testing/layers.md's L1a row: "every Tier 0-5 primitive has its own
// translation unit, its own table-driven tests ... exercised directly").
// Hand-rolled `main()`, no test framework, compiled and run with a bare
// `g++ -std=c++17` by `scripts/hygiene/kernel-primitive-tests.mjs` — the
// same dependency-free-TU pattern `window_test.cpp` established.
//
// Structure follows `docs/testing/primitives.md`'s Tier 1
// `lookback_bound / carry` row: the three named unit-contract cases (sample
// exactly at the bound, one microsecond older, a stale marker within
// lookback), the monotone-carry invariant, and the two named must-die
// mutants (boundary inclusivity, stale check removal).
#include "../../src/kernel/lookback.hpp"
#include "../../src/kernel/stale.hpp"

#include <cstdint>
#include <cstdio>
#include <random>

using chronoduck::carry;
using chronoduck::lookback_bound;
using chronoduck::stale_marker;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		std::fprintf(stderr, "lookback_test: FAIL — %s\n", what);
		g_failures++;
	}
}

// The reference's documented rule, transcribed as a table
// (docs/testing/primitives.md's own oracle column for this row): a sample
// carries iff it is at or before the anchor and no more than `lookback`
// microseconds older, and does not carry if it is the stale marker,
// regardless of position.
bool ReferenceRuleTable(int64_t anchor, int64_t lookback, int64_t sample_t, double sample_v) {
	if (chronoduck::is_stale(sample_v)) {
		return false;
	}
	return sample_t <= anchor && (anchor - sample_t) <= lookback;
}

// Unit contract: "Sample exactly at the bound (carried), one µs older
// (not), stale marker within lookback (not)."
void TestUnitContractCases() {
	const int64_t anchor = 1000;
	const int64_t lookback = 100;

	Check(lookback_bound(anchor, lookback) == 900, "lookback_bound(1000, 100) must be 900");

	Check(carry(anchor, lookback, 900, 42.0), "a sample exactly at the bound (900) must carry");
	Check(!carry(anchor, lookback, 899, 42.0), "a sample one microsecond older than the bound (899) must not carry");
	Check(carry(anchor, lookback, 1000, 42.0), "a sample at the anchor itself must carry");
	Check(!carry(anchor, lookback, 1001, 42.0), "a sample after the anchor must not carry");
	Check(!carry(anchor, lookback, 950, stale_marker()),
	      "a stale marker within the lookback window must not carry — the stale check ends the carry");
}

// Invariant: "Carry is monotone: if a sample carries at anchor a, it
// carries at any a' < a with a' >= t."
void TestMonotoneCarryInvariant() {
	std::mt19937_64 rng(0x5EED);
	std::uniform_int_distribution<int64_t> t_dist(0, 100000);
	std::uniform_int_distribution<int64_t> lookback_dist(0, 5000);
	std::uniform_int_distribution<int64_t> gap_dist(0, 5000); // anchor - t, kept within [0, lookback]

	for (int trial = 0; trial < 5000; trial++) {
		int64_t t = t_dist(rng);
		int64_t lookback = lookback_dist(rng);
		int64_t gap = gap_dist(rng) % (lookback + 1); // 0..lookback, so it carries at `a`
		int64_t a = t + gap;
		double v = 7.0; // not stale

		if (!carry(a, lookback, t, v)) {
			continue; // didn't satisfy the "carries at a" precondition, skip
		}
		// Pick a' with t <= a' < a.
		if (a == t) {
			continue; // no room for a strictly smaller a' >= t
		}
		std::uniform_int_distribution<int64_t> a_prime_dist(t, a - 1);
		int64_t a_prime = a_prime_dist(rng);
		Check(carry(a_prime, lookback, t, v),
		      "monotone carry: a sample that carries at anchor a must also carry at any a' < a with a' >= t");
	}
}

// This issue's Goal also names `scan_bounds` tested via folds; this
// primitive's own invariant is checked against the reference rule table
// directly, independently of the monotone-carry sweep above.
void TestAgreesWithReferenceRuleTable() {
	std::mt19937_64 rng(0xBEEF);
	std::uniform_int_distribution<int64_t> anchor_dist(-100000, 100000);
	std::uniform_int_distribution<int64_t> lookback_dist(0, 10000);
	std::uniform_int_distribution<int64_t> t_dist(-200000, 200000);

	for (int trial = 0; trial < 5000; trial++) {
		int64_t anchor = anchor_dist(rng);
		int64_t lookback = lookback_dist(rng);
		int64_t t = t_dist(rng);
		Check(carry(anchor, lookback, t, 1.0) == ReferenceRuleTable(anchor, lookback, t, 1.0),
		      "carry() must agree with the reference rule table for random anchor/lookback/t");
	}
}

// Must-die mutant #1: "Boundary inclusivity"
// (`docs/testing/primitives.md:lookback-row:` `Boundary inclusivity`).
// Flips the lower bound from `<=` to `<`, which wrongly excludes a sample
// exactly at the bound — the unit contract's own first case is the killing
// row.
bool MutantExclusiveBound(int64_t anchor, int64_t lookback, int64_t sample_t, double sample_v) {
	if (chronoduck::is_stale(sample_v)) {
		return false;
	}
	__int128_t bound = static_cast<__int128_t>(anchor) - static_cast<__int128_t>(lookback);
	__int128_t st = sample_t, aa = anchor;
	return bound < st && st <= aa; // flipped: was <=
}

void TestBoundaryInclusivityMutant() {
	Check(carry(1000, 100, 900, 42.0), "sanity: the real carry() includes the sample exactly at the bound");
	Check(!MutantExclusiveBound(1000, 100, 900, 42.0),
	      "must-die: the exclusive-bound mutant wrongly excludes the sample exactly at the bound (900)");
}

// Must-die mutant #2: "stale check removal"
// (`docs/testing/primitives.md:lookback-row:` `stale check removal`). Drops
// the `is_stale` guard entirely, so a stale marker within the lookback
// window wrongly carries.
bool MutantNoStaleCheck(int64_t anchor, int64_t lookback, int64_t sample_t) {
	__int128_t bound = static_cast<__int128_t>(anchor) - static_cast<__int128_t>(lookback);
	__int128_t st = sample_t, aa = anchor;
	return bound <= st && st <= aa; // no is_stale check at all
}

void TestStaleCheckRemovalMutant() {
	Check(!carry(1000, 100, 950, stale_marker()), "sanity: the real carry() rejects a stale marker within lookback");
	Check(MutantNoStaleCheck(1000, 100, 950),
	      "must-die: the stale-check-removed mutant wrongly carries a stale marker within lookback");
}

} // namespace

int main() {
	TestUnitContractCases();
	TestMonotoneCarryInvariant();
	TestAgreesWithReferenceRuleTable();
	TestBoundaryInclusivityMutant();
	TestStaleCheckRemovalMutant();

	if (g_failures > 0) {
		std::fprintf(stderr, "lookback_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("lookback_test: PASS\n");
	return 0;
}
