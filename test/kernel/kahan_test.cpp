// kahan_test.cpp — the L1a direct test for `src/kernel/kahan.hpp`
// (docs/testing/layers.md's L1a row: "every Tier 0-5 primitive has its own
// translation unit, its own table-driven tests ... exercised directly").
// Hand-rolled `main()`, no test framework, compiled and run with a bare
// `g++ -std=c++17` by `scripts/hygiene/kernel-primitive-tests.mjs` — the same
// dependency-free-TU pattern `comparator_test.cpp` established.
//
// Structure follows `docs/testing/primitives.md`'s Tier 0 `kahan_add /
// kahan_merge` row: a cancellation table checked against an independent
// exact-arithmetic oracle, the property invariants, and the three must-die
// mutants, each demonstrated by a "shadow" implementation carrying the
// mutation rather than only cited in prose — the same standard
// `docs/testing/comparator.md`'s own cancellation-gap demonstration set for
// this repo.
//
// The fourth item in the unit contract, "the FMA-defeating cast", is a
// codegen property of `src/kernel/kahan.hpp` itself (its own header comment
// explains the `volatile` intermediates), not one a unit test can
// independently reproduce — x86-64's SSE2 double arithmetic has no excess
// precision to defeat in the first place, so there is no observable
// behavioural difference to assert on here.
#include "../../src/kernel/comparator.hpp"
#include "../../src/kernel/kahan.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using chronoduck::equal_values;
using chronoduck::kahan_add;
using chronoduck::kahan_merge;
using chronoduck::KahanState;
using chronoduck::kUnitRoundoff;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		std::fprintf(stderr, "kahan_test: FAIL — %s\n", what);
		g_failures++;
	}
}

uint64_t Bits(double v) {
	uint64_t bits;
	std::memcpy(&bits, &v, sizeof(bits));
	return bits;
}

bool BitExact(double a, double b) {
	return Bits(a) == Bits(b);
}

bool BitExact(KahanState a, KahanState b) {
	return Bits(a.sum) == Bits(b.sum) && Bits(a.comp) == Bits(b.comp);
}

double Finalize(KahanState s) {
	return s.sum + s.comp;
}

double FoldKahan(const std::vector<double> &xs) {
	KahanState s;
	for (double x : xs)
		s = kahan_add(s, x);
	return Finalize(s);
}

// Must-die mutant #1: "dropping the compensation term"
// (`docs/testing/primitives.md:kahan-mutant-magnitude:` `swapping the magnitude test`
// names the other two; this one is AC1 itself, "cancellation table fails
// when compensation is removed" — a running total with no correction at all
// is exactly what kahan_add degenerates to if `comp` is deleted and never
// folded back in at the end).
double NaiveSum(const std::vector<double> &xs) {
	double total = 0.0;
	for (double x : xs)
		total += x;
	return total;
}

// The independent exact-arithmetic oracle: `docs/testing/primitives.md`'s
// "Exact rational arithmetic (a 128-bit or arbitrary-precision sum in the
// test)". This is a genuinely different algorithm from kahan.hpp's Neumaier
// variant — Knuth's 2Sum error-free transform accumulated into a
// double-double (`hi + lo`, ~106 bits of mantissa) — so it verifies
// kahan_add's answer instead of re-deriving the same formula, the same
// independence `comparator_test.cpp`'s re-derived `kReorderFactor`
// established for this repo.
struct ExactSum {
	double hi = 0.0;
	double lo = 0.0;

	void Add(double x) {
		double s = hi + x;
		double bb = s - hi;
		double err = (hi - (s - bb)) + (x - bb);
		hi = s;
		lo += err;
	}
	double Value() const {
		return hi + lo;
	}
};

double ExactOracle(const std::vector<double> &xs) {
	ExactSum acc;
	for (double x : xs)
		acc.Add(x);
	return acc.Value();
}

// A tight, answer-scaled bound — deliberately NOT `docs/testing/comparator.md`'s
// `kReorderFactor * Σ|terms|` (that bound is sized for *reordering* two
// already-correct sums against each other, and this document's own
// comparator page says outright that a cancelling `Σ|terms|`-scaled bound
// "still passes" a naive zero answer — precisely the gap this L1a table
// exists to catch, per that same page's next paragraph). Scaling by the
// *answer's* own magnitude instead of the cancelling terms' magnitude is
// what makes the naive/compensated comparison below actually discriminate.
double CancellationBound(double oracle_answer) {
	return 16.0 * kUnitRoundoff * std::max(1.0, std::fabs(oracle_answer));
}

struct CancellationCase {
	const char *name;
	std::vector<double> terms;
};

const CancellationCase kCancellationTable[] = {
    {"large, small, negated large: 1e16 + 1 - 1e16", {1e16, 1.0, -1e16}},
    {"large, ten small, negated large", {1e16, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1e16}},
    {"alternating huge cancellations around small terms", {1.0, 1e18, -1e18, 2.0, 1e18, -1e18, 3.0}},
};

// AC1: "Cancellation table fails when compensation is removed" — proven, not
// merely asserted: the compensated fold matches the independent oracle to
// within `CancellationBound`, and `NaiveSum` (compensation dropped) on the
// very same table entry misses that bound by orders of magnitude.
void TestCancellationTable() {
	for (const auto &c : kCancellationTable) {
		const double oracle = ExactOracle(c.terms);
		const double compensated = FoldKahan(c.terms);
		const double naive = NaiveSum(c.terms);
		const double bound = CancellationBound(oracle);

		char what[256];
		std::snprintf(what, sizeof(what), "%s: compensated (%.17g) must match the oracle (%.17g) within %.3g", c.name,
		              compensated, oracle, bound);
		Check(std::fabs(compensated - oracle) <= bound, what);

		std::snprintf(what, sizeof(what),
		              "%s: naive summation (compensation dropped, %.17g) must OBSERVABLY MISS the oracle (%.17g) — "
		              "this is the cancellation table failing when compensation is removed",
		              c.name, naive, oracle);
		Check(std::fabs(naive - oracle) > bound, what);
	}
}

// Subnormals get their own check (unit contract), separate from the
// naive-vs-compensated table above: the lost term here is astronomically
// small in absolute terms, so a naive/compensated *comparison* would not
// discriminate anything — what matters is that kahan_add still recovers the
// subnormal exactly rather than losing or corrupting it.
void TestSubnormal() {
	std::vector<double> terms = {1.0, std::numeric_limits<double>::denorm_min(), -1.0};
	const double oracle = ExactOracle(terms);
	const double compensated = FoldKahan(terms);
	Check(compensated == oracle, "subnormal term folded into a normal sum must be recovered exactly");
	Check(compensated == std::numeric_limits<double>::denorm_min(),
	      "the recovered value must equal denorm_min exactly");
}

// Must-die mutant #2: "removing the Inf special case"
// (`docs/design/primitives.md:tier0-row:` `(Neumaier, FMA-defeating, Inf-safe)`).
// Reproduces the mutant directly: the same Neumaier formula with the
// `std::isinf` guard deleted,
// which manufactures a NaN out of `Inf + finite` via `Inf - Inf` inside the
// correction term, where the real primitive stays `Inf`.
KahanState MutantNoInfGuard(KahanState state, double x) {
	const double old_sum = state.sum;
	const double t = old_sum + x;
	if (std::fabs(old_sum) >= std::fabs(x)) {
		state.comp += (old_sum - t) + x;
	} else {
		state.comp += (x - t) + old_sum;
	}
	state.sum = t;
	return state;
}

void TestInfSafety() {
	const double inf = std::numeric_limits<double>::infinity();

	KahanState both_pos_inf = kahan_add(kahan_add(KahanState {}, inf), inf);
	Check(Finalize(both_pos_inf) == inf, "+Inf + +Inf must finalize to +Inf");

	KahanState pos_then_neg = kahan_add(kahan_add(KahanState {}, inf), -inf);
	Check(std::isnan(Finalize(pos_then_neg)), "+Inf + -Inf must finalize to NaN (IEEE-754's own rule)");

	// The mutant: a large finite base plus one Inf term. The real primitive
	// must still finalize to +Inf (Inf + finite == Inf); the mutant, lacking
	// the Inf guard, corrupts its compensation term to NaN via `Inf - Inf`
	// inside the correction, and NaN then poisons the finalized answer even
	// though the mathematically correct answer is well-defined.
	KahanState real = kahan_add(kahan_add(KahanState {}, 1e300), inf);
	Check(Finalize(real) == inf, "kahan_add: a finite base plus +Inf must finalize to +Inf");

	KahanState mutant = MutantNoInfGuard(MutantNoInfGuard(KahanState {}, 1e300), inf);
	Check(std::isnan(Finalize(mutant)),
	      "must-die: removing the Inf special case turns a well-defined +Inf answer into NaN — "
	      "this mutant is observably wrong and this test kills it");
}

void TestNaNPropagation() {
	const double nan_value = std::numeric_limits<double>::quiet_NaN();
	KahanState s = kahan_add(KahanState {}, 1.0);
	s = kahan_add(s, nan_value);
	Check(std::isnan(Finalize(s)), "a NaN term must propagate to a NaN finalized answer");
}

// Must-die mutant #3: "swapping the magnitude test"
// (`src/kernel/kahan.hpp:kahan_add:` `swapping the magnitude test`).
// Reproduces the mutant directly: `>=` flipped to `<`, so the correction
// formula is applied with the roles reversed for every call — wrong for the
// half of all additions where `|old_sum| >= |x|` actually holds.
KahanState MutantFlippedMagnitudeTest(KahanState state, double x) {
	const double old_sum = state.sum;
	const double t = old_sum + x;
	if (std::isinf(t) || std::isinf(old_sum) || std::isinf(x)) {
		state.sum = t;
		return state;
	}
	if (std::fabs(old_sum) < std::fabs(x)) { // flipped: was `>=` selecting the other branch
		state.comp += (old_sum - t) + x;
	} else {
		state.comp += (x - t) + old_sum;
	}
	state.sum = t;
	return state;
}

double FoldMutant(const std::vector<double> &xs, KahanState (*add)(KahanState, double)) {
	KahanState s;
	for (double x : xs)
		s = add(s, x);
	return Finalize(s);
}

void TestMagnitudeTestMutant() {
	for (const auto &c : kCancellationTable) {
		const double oracle = ExactOracle(c.terms);
		const double mutant = FoldMutant(c.terms, MutantFlippedMagnitudeTest);
		const double bound = CancellationBound(oracle);
		char what[256];
		std::snprintf(what, sizeof(what),
		              "%s: flipping the magnitude test must diverge from the oracle beyond %.3g (got %.17g vs %.17g)",
		              c.name, bound, mutant, oracle);
		Check(std::fabs(mutant - oracle) > bound, what);
	}
}

// Invariant: "Sum of terms in any order agrees within the reorder bound" —
// this one legitimately IS `docs/testing/comparator.md`'s shared derivation:
// unlike the cancellation table above (which is about a *known-bad*
// algorithm losing an answer's own magnitude of precision), reordering an
// already-compensated fold is exactly the class the reorder bound is sized
// for, so this test reuses `equal_values` rather than inventing a second
// tolerance (`docs/testing/rules.md`'s T3, "one comparator ... applied to
// every value").
void TestAnyOrderAgreesWithinReorderBound() {
	const std::vector<std::vector<double>> orderings = {
	    {3.5, -100.25, 0.001, 7.0, -3.5, 42.125, -0.001},
	    {-100.25, 7.0, 3.5, -3.5, 42.125, 0.001, -0.001},
	    {42.125, 0.001, -3.5, -100.25, -0.001, 3.5, 7.0},
	    {-0.001, -3.5, 42.125, 3.5, 7.0, -100.25, 0.001},
	};
	double scale_abs_sum = 0.0;
	for (double x : orderings[0])
		scale_abs_sum += std::fabs(x);

	double first = FoldKahan(orderings[0]);
	for (std::size_t i = 1; i < orderings.size(); i++) {
		double other = FoldKahan(orderings[i]);
		Check(equal_values(first, other, scale_abs_sum),
		      "kahan_add: sums of a reordered term set must agree within the reorder bound");
	}
}

// Invariant: "sum of sorted terms is bit-identical across runs" — the same
// fixed, sorted sequence folded repeatedly must produce the exact same bits
// every time (no dependence on uninitialized memory, allocation addresses,
// or anything else that isn't the input itself).
void TestSortedInputBitIdenticalAcrossRuns() {
	std::vector<double> sorted_terms = {-50.0, -12.5, -1.0, 0.25, 3.75, 9.0, 20.5, 100.0};
	double first_run = FoldKahan(sorted_terms);
	for (int run = 0; run < 8; run++) {
		double this_run = FoldKahan(sorted_terms);
		Check(BitExact(first_run, this_run), "sorted-input sum must be bit-identical across repeated runs");
	}
}

// Invariant: "merge(a,b) == merge(b,a) bit-exact" — including a case where
// `a.sum` and `b.sum` tie exactly in magnitude, which is what
// `detail::KahanOutranks`'s bit-pattern tie-break inside kahan.hpp exists
// for.
void TestMergeCommutative() {
	const KahanState pairs[][2] = {
	    {KahanState {5.0, 0.25}, KahanState {-3.0, 0.1}},
	    {KahanState {1e16, 1.0}, KahanState {-1e16, 0.0}},
	    {KahanState {7.0, 0.0}, KahanState {-7.0, 0.5}}, // magnitude tie, opposite sign
	    {KahanState {2.5, 0.0}, KahanState {2.5, 0.0}},  // magnitude tie, identical
	};
	for (const auto &p : pairs) {
		KahanState ab = kahan_merge(p[0], p[1]);
		KahanState ba = kahan_merge(p[1], p[0]);
		Check(BitExact(ab, ba), "kahan_merge(a, b) must equal kahan_merge(b, a) bit-exact");
	}
}

// Invariant: "merge of a partial with the zero state is identity".
void TestMergeZeroIdentity() {
	KahanState a {123.456, 0.0001};
	KahanState zero {};
	Check(BitExact(kahan_merge(a, zero), a), "merge(a, zero) must equal a bit-exact");
	Check(BitExact(kahan_merge(zero, a), a), "merge(zero, a) must equal a bit-exact");
}

} // namespace

int main() {
	TestCancellationTable();
	TestSubnormal();
	TestInfSafety();
	TestNaNPropagation();
	TestMagnitudeTestMutant();
	TestAnyOrderAgreesWithinReorderBound();
	TestSortedInputBitIdenticalAcrossRuns();
	TestMergeCommutative();
	TestMergeZeroIdentity();

	if (g_failures > 0) {
		std::fprintf(stderr, "kahan_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("kahan_test: PASS\n");
	return 0;
}
