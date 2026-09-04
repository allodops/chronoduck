// scan_bounds_test.cpp — the L1a direct test for
// `src/kernel/scan_bounds.hpp` (docs/testing/layers.md's L1a row: "every
// Tier 0-5 primitive has its own translation unit, its own table-driven
// tests ... exercised directly"). Hand-rolled `main()`, no test framework,
// compiled and run with a bare `g++ -std=c++17` by
// `scripts/hygiene/kernel-primitive-tests.mjs` — the same
// dependency-free-TU pattern `lookback_test.cpp` established.
//
// Structure follows `docs/testing/primitives.md`'s Tier 1 `scan_bounds` row
// exactly: the per-edge-mode unit-contract table, the invariant — this
// issue's own "scan_bounds tested by folds over bounded vs unbounded data
// with an excluded sample present" — checked with a small hand-rolled fold
// per edge mode (real folds are Tier 4's, out of this issue's scope; these
// stand-ins implement just enough of each edge mode's read pattern to prove
// scan_bounds's own coverage claim), and the one named must-die mutant
// ("dropping a term of the lower bound"), demonstrated exactly the way the
// document says it must be: by asserting which rows fall inside the bound,
// not by comparing fold output values (a fixture where the dropped term
// happens not to change the answer would let a naive, value-only test miss
// it).
#include "../../src/kernel/grid.hpp"
#include "../../src/kernel/lookback.hpp"
#include "../../src/kernel/scan_bounds.hpp"
#include "../../src/kernel/window.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

using namespace chronoduck;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		std::fprintf(stderr, "scan_bounds_test: FAIL — %s\n", what);
		g_failures++;
	}
}

struct Sample {
	int64_t t;
	double v;
};

std::vector<Sample> Slice(const std::vector<Sample> &all, int64_t lower, int64_t upper) {
	std::vector<Sample> out;
	for (const auto &s : all) {
		if (s.t >= lower && s.t <= upper) {
			out.push_back(s);
		}
	}
	return out;
}

// INSIDE's own read: sum of every sample `Window{anchor,window}` contains —
// the simplest possible stand-in for "any fold on this grid", since every
// edge mode's own read is a superset of this one.
double InsideFold(int64_t anchor, int64_t window, const std::vector<Sample> &samples) {
	Window w {anchor, window};
	double sum = 0.0;
	for (const auto &s : samples) {
		if (w.contains(s.t)) {
			sum += s.v;
		}
	}
	return sum;
}

// LOOKBACK's own read: the primary window if non-empty, else the carry rule
// applied at the window's own left edge (`docs/design/architecture.md:edge-modes:`
// "carry within lookback").
double LookbackFold(int64_t anchor, int64_t window, int64_t lookback, const std::vector<Sample> &samples) {
	Window w {anchor, window};
	bool any_inside = false;
	for (const auto &s : samples) {
		if (w.contains(s.t)) {
			any_inside = true;
			break;
		}
	}
	if (any_inside) {
		return InsideFold(anchor, window, samples);
	}
	int64_t left_edge = anchor - window;
	bool found = false;
	double val = 0.0;
	int64_t best_t = std::numeric_limits<int64_t>::min();
	for (const auto &s : samples) {
		if (carry(left_edge, lookback, s.t, s.v) && s.t > best_t) {
			best_t = s.t;
			val = s.v;
			found = true;
		}
	}
	return found ? val : 0.0;
}

// ANCHOR's own read: the last sample at or before the window's left edge,
// searched up to `lookback` beyond it — never the primary window at all
// (`docs/design/architecture.md:edge-modes:` "read the last sample before the window").
double AnchorFold(int64_t anchor, int64_t window, int64_t lookback, const std::vector<Sample> &samples) {
	int64_t left_edge = anchor - window;
	bool found = false;
	double val = 0.0;
	int64_t best_t = std::numeric_limits<int64_t>::min();
	for (const auto &s : samples) {
		if (carry(left_edge, lookback, s.t, s.v) && s.t > best_t) {
			best_t = s.t;
			val = s.v;
			found = true;
		}
	}
	return found ? val : 0.0;
}

// SMOOTH's own read: the last sample inside the primary window (the left
// edge, needing no reach beyond `-window` — this issue's scan_bounds
// deliberately gives SMOOTH no extra lower-bound term, see
// `src/kernel/scan_bounds.hpp`'s own header comment) plus the first sample
// after `anchor` within `lookback` (the right edge, which is exactly what
// the `+lookahead` upper-bound term exists to reach).
double SmoothFold(int64_t anchor, int64_t window, int64_t lookback, const std::vector<Sample> &samples) {
	Window w {anchor, window};
	bool left_found = false;
	double left_val = 0.0;
	int64_t left_best_t = std::numeric_limits<int64_t>::min();
	for (const auto &s : samples) {
		if (w.contains(s.t) && s.t > left_best_t) {
			left_best_t = s.t;
			left_val = s.v;
			left_found = true;
		}
	}
	bool right_found = false;
	double right_val = 0.0;
	int64_t right_best_t = std::numeric_limits<int64_t>::max();
	for (const auto &s : samples) {
		if (s.t > anchor && s.t <= anchor + lookback && s.t < right_best_t) {
			right_best_t = s.t;
			right_val = s.v;
			right_found = true;
		}
	}
	return (left_found ? left_val : 0.0) + (right_found ? right_val : 0.0);
}

// Unit contract: per docs/testing/primitives.md's own table — "INSIDE →
// [start − window, end]; +lookback for LOOKBACK; −extra for ANCHOR;
// +lookahead for SMOOTH" — verified as exact numbers on one concrete grid.
void TestUnitContractTable() {
	Grid grid(1000, 3000, 1000); // points at 1000, 2000, 3000
	const int64_t window = 200;
	const int64_t lookback = 150;

	ScanBounds inside = scan_bounds(grid, window, lookback, INSIDE);
	Check(inside.lower == 800 && inside.upper == 3000, "INSIDE bound must be [start - window, end] = [800, 3000]");

	ScanBounds lb = scan_bounds(grid, window, lookback, LOOKBACK);
	Check(lb.lower == 650 && lb.upper == 3000, "LOOKBACK bound must be [start - window - lookback, end] = [650, 3000]");

	ScanBounds anchor = scan_bounds(grid, window, lookback, ANCHOR);
	Check(anchor.lower == 650 && anchor.upper == 3000,
	      "ANCHOR bound must be [start - window - lookback, end] = [650, 3000] (this issue's deviation: ANCHOR "
	      "reuses lookback as its own extra magnitude)");

	ScanBounds smooth = scan_bounds(grid, window, lookback, SMOOTH);
	Check(smooth.lower == 800 && smooth.upper == 3150,
	      "SMOOTH bound must be [start - window, end + lookback] = [800, 3150]");

	// Combining bits is additive, never a max — always at least as wide as
	// either bit alone, which is what keeps the result a safe superset.
	ScanBounds combined = scan_bounds(grid, window, lookback, LOOKBACK | ANCHOR);
	Check(combined.lower == 500, "LOOKBACK|ANCHOR together subtract lookback twice: 1000 - 200 - 150 - 150 = 500");
	Check(combined.lower <= lb.lower, "the combined bound must be at least as wide as LOOKBACK alone");
}

// The invariant: "Every sample that any fold on this grid could read lies
// inside the bounds" — checked per edge mode by running the mode's own fold
// over the full sample set and over only the samples scan_bounds would
// admit, and asserting they agree, with at least one sample genuinely
// outside the bound present in the full set.
void TestInsideModeInvariant() {
	Grid grid(1000, 3000, 1000);
	const int64_t window = 200, lookback = 150;
	ScanBounds b = scan_bounds(grid, window, lookback, INSIDE);

	std::vector<Sample> all = {
	    {900, 1.0},  // inside grid point 1000's window (800, 1000]
	    {1900, 2.0}, // inside grid point 2000's window
	    {2900, 3.0}, // inside grid point 3000's window
	    {50, 99.0},  // well outside the bound [800, 3000] — the excluded sample
	};
	bool any_excluded = false;
	for (const auto &s : all) {
		if (s.t < b.lower || s.t > b.upper) {
			any_excluded = true;
		}
	}
	Check(any_excluded, "INSIDE: at least one sample must lie outside the scan bound");

	std::vector<Sample> bounded = Slice(all, b.lower, b.upper);
	for (int64_t i = 0; i < grid.count(); i++) {
		int64_t anchor = grid.at(i);
		Check(InsideFold(anchor, window, all) == InsideFold(anchor, window, bounded),
		      "INSIDE: the fold over the full data must equal the fold over only the bounded data");
	}
}

void TestLookbackModeInvariant() {
	Grid grid(1000, 3000, 1000);
	const int64_t window = 200, lookback = 150;
	ScanBounds b = scan_bounds(grid, window, lookback, LOOKBACK);

	std::vector<Sample> all = {
	    {700, 5.0},   // carries into grid point 1000's empty window via lookback
	    {1900, 6.0},  // inside grid point 2000's own window — no carry needed
	    {2900, 7.0},  // inside grid point 3000's own window
	    {500, 999.0}, // well outside the bound [650, 3000] — the excluded sample
	};
	bool any_excluded = false;
	for (const auto &s : all) {
		if (s.t < b.lower || s.t > b.upper) {
			any_excluded = true;
		}
	}
	Check(any_excluded, "LOOKBACK: at least one sample must lie outside the scan bound");

	std::vector<Sample> bounded = Slice(all, b.lower, b.upper);
	for (int64_t i = 0; i < grid.count(); i++) {
		int64_t anchor = grid.at(i);
		Check(LookbackFold(anchor, window, lookback, all) == LookbackFold(anchor, window, lookback, bounded),
		      "LOOKBACK: the fold over the full data must equal the fold over only the bounded data");
	}
	// Sanity: the carry actually fired (grid point 1000's window was empty).
	Check(LookbackFold(1000, window, lookback, all) == 5.0, "sanity: grid point 1000 must carry the value 5.0");
}

void TestAnchorModeInvariant() {
	Grid grid(1000, 3000, 1000);
	const int64_t window = 200, lookback = 150;
	ScanBounds b = scan_bounds(grid, window, lookback, ANCHOR);

	std::vector<Sample> all = {
	    {750, 11.0},  // anchors grid point 1000 (left edge 800, within lookback 150 of it)
	    {1750, 12.0}, // anchors grid point 2000 (left edge 1800)
	    {2750, 13.0}, // anchors grid point 3000 (left edge 2800)
	    {100, 999.0}, // well outside the bound [650, 3000] — the excluded sample
	};
	bool any_excluded = false;
	for (const auto &s : all) {
		if (s.t < b.lower || s.t > b.upper) {
			any_excluded = true;
		}
	}
	Check(any_excluded, "ANCHOR: at least one sample must lie outside the scan bound");

	std::vector<Sample> bounded = Slice(all, b.lower, b.upper);
	for (int64_t i = 0; i < grid.count(); i++) {
		int64_t anchor = grid.at(i);
		Check(AnchorFold(anchor, window, lookback, all) == AnchorFold(anchor, window, lookback, bounded),
		      "ANCHOR: the fold over the full data must equal the fold over only the bounded data");
	}
	Check(AnchorFold(1000, window, lookback, all) == 11.0, "sanity: grid point 1000 must anchor on 11.0");
}

void TestSmoothModeInvariant() {
	Grid grid(1000, 3000, 1000);
	const int64_t window = 200, lookback = 150;
	ScanBounds b = scan_bounds(grid, window, lookback, SMOOTH);

	std::vector<Sample> all = {
	    {2950, 20.0},  // left edge of grid point 3000's window
	    {3100, 21.0},  // right of grid point 3000, within lookback — needs the +lookahead reach
	    {1950, 30.0},  // left edge of grid point 2000's window
	    {3200, 999.0}, // beyond the bound [800, 3150] — the excluded sample
	};
	bool any_excluded = false;
	for (const auto &s : all) {
		if (s.t < b.lower || s.t > b.upper) {
			any_excluded = true;
		}
	}
	Check(any_excluded, "SMOOTH: at least one sample must lie outside the scan bound");

	std::vector<Sample> bounded = Slice(all, b.lower, b.upper);
	for (int64_t i = 0; i < grid.count(); i++) {
		int64_t anchor = grid.at(i);
		Check(SmoothFold(anchor, window, lookback, all) == SmoothFold(anchor, window, lookback, bounded),
		      "SMOOTH: the fold over the full data must equal the fold over only the bounded data");
	}
	Check(SmoothFold(3000, window, lookback, all) == 41.0,
	      "sanity: grid point 3000 must combine both edges (20.0 + 21.0)");
}

// Must-die mutant: "Dropping a term of the lower bound"
// (`docs/testing/primitives.md:scan-bounds-row:` "Dropping a term of the
// lower bound — the mutant produces correct output and only more rows
// read, so the test must assert rows read, not values"). This reproduces
// exactly that shape: a crafted carry sample that a naive value-only
// comparison could miss (see the sanity value check below, which happens to
// also catch it here), so the test asserts the structural property the
// document calls for directly — which rows scan_bounds admits — rather than
// relying on the fold's arithmetic to notice.
ScanBounds MutantDropLookbackTerm(const Grid &grid, int64_t window, int64_t lookback, unsigned mode) {
	int64_t lower = grid.start - window; // the LOOKBACK "- lookback" term is dropped
	int64_t upper = grid.end;
	if (mode & SMOOTH) {
		upper += lookback;
	}
	return ScanBounds {lower, upper};
}

void TestDroppedLowerBoundTermMutant() {
	Grid grid(1000, 3000, 1000);
	const int64_t window = 200, lookback = 150;

	ScanBounds real = scan_bounds(grid, window, lookback, LOOKBACK);
	ScanBounds mutant = MutantDropLookbackTerm(grid, window, lookback, LOOKBACK);
	Check(real.lower == 650, "sanity: the real LOOKBACK lower bound is 650");
	Check(mutant.lower == 800, "sanity: the term-dropping mutant's lower bound is 800");

	// The killing row: a carry sample the real bound admits but the mutant
	// excludes — asserted on inclusion (rows read), not on any fold's
	// output value, per the document's own instruction for this mutant.
	const int64_t carry_sample_t = 700;
	Check(carry_sample_t >= real.lower && carry_sample_t <= real.upper,
	      "must-die: the real scan bound must admit the carry sample at t=700");
	Check(!(carry_sample_t >= mutant.lower && carry_sample_t <= mutant.upper),
	      "must-die: the term-dropping mutant must NOT admit the same carry sample at t=700");

	// Incidental confirmation that this particular fixture also happens to
	// diverge in value (not every fixture would; the structural assertion
	// above is what the document requires, this is a bonus).
	std::vector<Sample> all = {{carry_sample_t, 5.0}};
	Check(LookbackFold(1000, window, lookback, Slice(all, real.lower, real.upper)) == 5.0,
	      "the correctly-bounded fold recovers the carried value 5.0");
	Check(LookbackFold(1000, window, lookback, Slice(all, mutant.lower, mutant.upper)) == 0.0,
	      "must-die: the mutant-bounded fold misses the carried value entirely (reads 0 rows instead of 1)");
}

} // namespace

int main() {
	TestUnitContractTable();
	TestInsideModeInvariant();
	TestLookbackModeInvariant();
	TestAnchorModeInvariant();
	TestSmoothModeInvariant();
	TestDroppedLowerBoundTermMutant();

	if (g_failures > 0) {
		std::fprintf(stderr, "scan_bounds_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("scan_bounds_test: PASS\n");
	return 0;
}
