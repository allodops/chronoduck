// window_test.cpp — the L1a direct test for `src/kernel/window.hpp`
// (docs/testing/layers.md's L1a row: "every Tier 0-5 primitive has its own
// translation unit, its own table-driven tests ... exercised directly").
// Hand-rolled `main()`, no test framework, compiled and run with a bare
// `g++ -std=c++17` by `scripts/hygiene/kernel-primitive-tests.mjs` — the
// same dependency-free-TU pattern `grid_test.cpp` established.
//
// Structure follows `docs/testing/primitives.md`'s Tier 1 `Window.contains`
// row exactly: the six named edge cases, the invariant checked against an
// independently-spelled inequality, and — this issue's own acceptance
// criterion — a killing row for *every* `<`/`<=` the function contains.
#include "../../src/kernel/window.hpp"

#include <cstdint>
#include <cstdio>
#include <random>

using chronoduck::Window;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		std::fprintf(stderr, "window_test: FAIL — %s\n", what);
		g_failures++;
	}
}

// The independent oracle: the inequality `anchor - width < t <= anchor`,
// spelled a second time here with plain `long double` arithmetic — a
// different code path from `window.hpp`'s `__int128_t` comparison
// (docs/testing/primitives.md's own column for this row: "The inequality
// itself, in a second spelling").
bool OracleContains(int64_t anchor, int64_t width, int64_t t) {
	long double lower = static_cast<long double>(anchor) - static_cast<long double>(width);
	long double tt = static_cast<long double>(t);
	long double aa = static_cast<long double>(anchor);
	return lower < tt && tt <= aa;
}

// The six named edge cases (docs/testing/primitives.md's own unit contract
// for this row).
void TestSixEdgeCases() {
	Window w {100, 10}; // (90, 100]

	Check(!w.contains(90), "edge case: t == anchor - width (90) must be OUT");
	Check(w.contains(91), "edge case: t == anchor - width + 1us (91) must be IN");
	Check(w.contains(100), "edge case: t == anchor (100) must be IN");
	Check(!w.contains(101), "edge case: t == anchor + 1us (101) must be OUT");

	Window zero_width {100, 0}; // (100, 100]: empty, no integer t satisfies it
	Check(!zero_width.contains(100), "edge case: width 0 excludes even t == anchor");
	Check(!zero_width.contains(99), "edge case: width 0 excludes everything below anchor too");
	Check(!zero_width.contains(101), "edge case: width 0 excludes everything above anchor too");

	// Width larger than the series: a width that dwarfs any realistic
	// sample spread must still behave like any other width — nothing
	// special-cased for "large".
	Window huge_width {1000, 1000000000};
	Check(huge_width.contains(1), "edge case: width larger than the series still includes a very old in-range t");
	Check(!huge_width.contains(1000 - 1000000000), "edge case: exactly at anchor - huge_width is still OUT");
}

// Invariant: for random anchor, width, t: contains <=> anchor - width < t <=
// anchor, checked against the independently-spelled oracle.
void TestInvariantAgainstOracle() {
	std::mt19937_64 rng(0xA11CE);
	std::uniform_int_distribution<int64_t> anchor_dist(-1000000, 1000000);
	std::uniform_int_distribution<int64_t> width_dist(0, 2000000);
	std::uniform_int_distribution<int64_t> t_dist(-3000000, 3000000);

	for (int trial = 0; trial < 5000; trial++) {
		int64_t anchor = anchor_dist(rng);
		int64_t width = width_dist(rng);
		int64_t t = t_dist(rng);
		Window w {anchor, width};
		Check(w.contains(t) == OracleContains(anchor, width, t),
		      "Window::contains must agree with the independently-spelled inequality for random anchor/width/t");
	}
}

// This issue's own acceptance criterion: "Every < / <= in Window.contains
// has a killing row." `contains` has exactly two relational operators:
// `lower < tt` and `tt <= aa`. Each mutant below flips exactly one, and the
// two edge-case rows above (t == anchor - width, and t == anchor) are
// exactly the killing rows: the mutant that flips the lower bound is killed
// by t == anchor - width; the mutant that flips the upper bound is killed by
// t == anchor.
bool MutantLowerInclusive(int64_t anchor, int64_t width, int64_t t) {
	// `lower < tt` mutated to `lower <= tt`.
	__int128_t lower = static_cast<__int128_t>(anchor) - static_cast<__int128_t>(width);
	__int128_t tt = t, aa = anchor;
	return lower <= tt && tt <= aa;
}

bool MutantUpperExclusive(int64_t anchor, int64_t width, int64_t t) {
	// `tt <= aa` mutated to `tt < aa`.
	__int128_t lower = static_cast<__int128_t>(anchor) - static_cast<__int128_t>(width);
	__int128_t tt = t, aa = anchor;
	return lower < tt && tt < aa;
}

void TestEveryOperatorHasAKillingRow() {
	Window w {100, 10}; // (90, 100]

	// Killing row for `lower < tt` (mutated to `<=`): t == anchor - width
	// (90). Real: OUT. Mutant: IN. These must disagree.
	Check(!w.contains(90), "killing row for the lower-bound operator: the real function must say OUT at t=90");
	Check(MutantLowerInclusive(100, 10, 90),
	      "must-die: the lower-bound-inclusive mutant says IN at t=90, disagreeing with the real function");

	// Killing row for `tt <= aa` (mutated to `<`): t == anchor (100). Real:
	// IN. Mutant: OUT. These must disagree.
	Check(w.contains(100), "killing row for the upper-bound operator: the real function must say IN at t=100");
	Check(!MutantUpperExclusive(100, 10, 100),
	      "must-die: the upper-bound-exclusive mutant says OUT at t=100, disagreeing with the real function");
}

} // namespace

int main() {
	TestSixEdgeCases();
	TestInvariantAgainstOracle();
	TestEveryOperatorHasAKillingRow();

	if (g_failures > 0) {
		std::fprintf(stderr, "window_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("window_test: PASS\n");
	return 0;
}
