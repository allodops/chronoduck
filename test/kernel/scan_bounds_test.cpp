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
// with an excluded sample present" — checked per edge mode by running a
// small hand-rolled fold (real folds are Tier 4's, out of this issue's
// scope; these stand-ins implement just enough of each edge mode's read
// pattern to prove scan_bounds's own coverage claim) over many
// `std::mt19937_64`-randomized trials, the same standard `grid_test.cpp`,
// `window_test.cpp` and `lookback_test.cpp` hold themselves to for their own
// invariants (`docs/testing/primitives.md:scan-bounds-row:` `checked by
// running the fold on random data with and without the bound and asserting
// identical output`) — each trial also forcing a sample genuinely outside
// the bound so "at least one excluded sample existed" holds every time, not
// by chance. A fifth invariant test exercises a *combined* `EdgeMode`
// bitmask (`LOOKBACK | ANCHOR`) folding real data, not just the bound
// arithmetic `TestUnitContractTable`'s combined case already checks — the
// header comment's own justification for additive (never max) combination
// is that correctness must hold "even when a caller sets more than one
// bit". The one named must-die mutant ("dropping a term of the lower
// bound") is demonstrated exactly the way the document says it must be: by
// asserting which rows fall inside the bound, not by comparing fold output
// values (a fixture where the dropped term happens not to change the
// answer would let a naive, value-only test miss it).
#include "kernel/grid.hpp"
#include "kernel/lookback.hpp"
#include "kernel/scan_bounds.hpp"
#include "kernel/window.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
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

// A row that declares *both* LOOKBACK and ANCHOR needs whatever either
// mode's own read alone would need — this stand-in is the sum of both, so
// the invariant below proves the combined bound (`scan_bounds.hpp`'s
// additive, never-max combination) is a safe superset for both reads at
// once, not just for either one in isolation.
double CombinedLookbackAnchorFold(int64_t anchor, int64_t window, int64_t lookback,
                                  const std::vector<Sample> &samples) {
	return LookbackFold(anchor, window, lookback, samples) + AnchorFold(anchor, window, lookback, samples);
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

// Shared randomized-trial machinery for the per-mode invariant tests below.
// Each trial gets its own random grid, window, lookback, and sample set,
// with one sample deliberately forced outside the computed bound (on a
// randomly chosen side) so "at least one excluded sample existed"
// (`docs/testing/primitives.md:scan-bounds-row:` `checked by running the
// fold on random data with and without the bound and asserting identical
// output`) holds on every single trial, not merely by chance of the random
// draw.
struct RandomTrial {
	Grid grid;
	int64_t window;
	int64_t lookback;
	std::vector<Sample> samples;
};

RandomTrial MakeRandomTrial(std::mt19937_64 &rng, unsigned mode) {
	std::uniform_int_distribution<int64_t> start_dist(-2000, 2000);
	std::uniform_int_distribution<int64_t> step_dist(1, 40);
	std::uniform_int_distribution<int64_t> points_dist(1, 5);
	std::uniform_int_distribution<int64_t> window_dist(1, 120);
	std::uniform_int_distribution<int64_t> lookback_dist(0, 120);
	std::uniform_int_distribution<int64_t> sample_count_dist(3, 12);
	std::uniform_int_distribution<int64_t> value_dist(-1000, 1000);
	std::uniform_int_distribution<int64_t> side_dist(0, 1);

	int64_t start = start_dist(rng);
	int64_t step = step_dist(rng);
	int64_t points = points_dist(rng);
	int64_t end = start + step * (points - 1);
	Grid grid(start, end, step);
	int64_t window = window_dist(rng);
	int64_t lookback = lookback_dist(rng);

	ScanBounds b = scan_bounds(grid, window, lookback, mode);

	// A margin generous enough that random samples land on both sides of
	// the bound (well before it, well after it, and inside it), and that
	// forcing a sample `margin` beyond the bound can never wrap or land
	// back inside it.
	int64_t margin = (end - start) + window + lookback + step + 10;
	std::uniform_int_distribution<int64_t> t_dist(start - 3 * margin, end + 3 * margin);

	std::vector<Sample> samples;
	int64_t n = sample_count_dist(rng);
	for (int64_t i = 0; i < n; i++) {
		samples.push_back({t_dist(rng), static_cast<double>(value_dist(rng))});
	}
	if (side_dist(rng) == 0) {
		samples.push_back({b.lower - margin, static_cast<double>(value_dist(rng))});
	} else {
		samples.push_back({b.upper + margin, static_cast<double>(value_dist(rng))});
	}

	return RandomTrial {grid, window, lookback, samples};
}

const int kTrialsPerMode = 500;

// The invariant: "Every sample that any fold on this grid could read lies
// inside the bounds" — checked per edge mode across many randomized trials
// by running the mode's own fold over the full sample set and over only the
// samples scan_bounds would admit, and asserting they agree, with at least
// one sample genuinely outside the bound present in the full set of every
// trial.
void TestInsideModeInvariant() {
	std::mt19937_64 rng(0xB0A7);
	for (int trial = 0; trial < kTrialsPerMode; trial++) {
		RandomTrial rt = MakeRandomTrial(rng, INSIDE);
		ScanBounds b = scan_bounds(rt.grid, rt.window, rt.lookback, INSIDE);

		bool any_excluded = false;
		for (const auto &s : rt.samples) {
			if (s.t < b.lower || s.t > b.upper) {
				any_excluded = true;
			}
		}
		Check(any_excluded, "INSIDE: at least one sample must lie outside the scan bound");

		std::vector<Sample> bounded = Slice(rt.samples, b.lower, b.upper);
		for (int64_t i = 0; i < rt.grid.count(); i++) {
			int64_t anchor = rt.grid.at(i);
			Check(InsideFold(anchor, rt.window, rt.samples) == InsideFold(anchor, rt.window, bounded),
			      "INSIDE: the fold over the full data must equal the fold over only the bounded data");
		}
	}
}

void TestLookbackModeInvariant() {
	std::mt19937_64 rng(0xCA55E77E);
	for (int trial = 0; trial < kTrialsPerMode; trial++) {
		RandomTrial rt = MakeRandomTrial(rng, LOOKBACK);
		ScanBounds b = scan_bounds(rt.grid, rt.window, rt.lookback, LOOKBACK);

		bool any_excluded = false;
		for (const auto &s : rt.samples) {
			if (s.t < b.lower || s.t > b.upper) {
				any_excluded = true;
			}
		}
		Check(any_excluded, "LOOKBACK: at least one sample must lie outside the scan bound");

		std::vector<Sample> bounded = Slice(rt.samples, b.lower, b.upper);
		for (int64_t i = 0; i < rt.grid.count(); i++) {
			int64_t anchor = rt.grid.at(i);
			Check(LookbackFold(anchor, rt.window, rt.lookback, rt.samples) ==
			          LookbackFold(anchor, rt.window, rt.lookback, bounded),
			      "LOOKBACK: the fold over the full data must equal the fold over only the bounded data");
		}
	}
}

void TestAnchorModeInvariant() {
	std::mt19937_64 rng(0xACE0);
	for (int trial = 0; trial < kTrialsPerMode; trial++) {
		RandomTrial rt = MakeRandomTrial(rng, ANCHOR);
		ScanBounds b = scan_bounds(rt.grid, rt.window, rt.lookback, ANCHOR);

		bool any_excluded = false;
		for (const auto &s : rt.samples) {
			if (s.t < b.lower || s.t > b.upper) {
				any_excluded = true;
			}
		}
		Check(any_excluded, "ANCHOR: at least one sample must lie outside the scan bound");

		std::vector<Sample> bounded = Slice(rt.samples, b.lower, b.upper);
		for (int64_t i = 0; i < rt.grid.count(); i++) {
			int64_t anchor = rt.grid.at(i);
			Check(AnchorFold(anchor, rt.window, rt.lookback, rt.samples) ==
			          AnchorFold(anchor, rt.window, rt.lookback, bounded),
			      "ANCHOR: the fold over the full data must equal the fold over only the bounded data");
		}
	}
}

void TestSmoothModeInvariant() {
	std::mt19937_64 rng(0x5300714);
	for (int trial = 0; trial < kTrialsPerMode; trial++) {
		RandomTrial rt = MakeRandomTrial(rng, SMOOTH);
		ScanBounds b = scan_bounds(rt.grid, rt.window, rt.lookback, SMOOTH);

		bool any_excluded = false;
		for (const auto &s : rt.samples) {
			if (s.t < b.lower || s.t > b.upper) {
				any_excluded = true;
			}
		}
		Check(any_excluded, "SMOOTH: at least one sample must lie outside the scan bound");

		std::vector<Sample> bounded = Slice(rt.samples, b.lower, b.upper);
		for (int64_t i = 0; i < rt.grid.count(); i++) {
			int64_t anchor = rt.grid.at(i);
			Check(SmoothFold(anchor, rt.window, rt.lookback, rt.samples) ==
			          SmoothFold(anchor, rt.window, rt.lookback, bounded),
			      "SMOOTH: the fold over the full data must equal the fold over only the bounded data");
		}
	}
}

// The invariant, exercised for a *combined* bitmask on real data: a row
// declaring `LOOKBACK | ANCHOR` needs both reads covered by one bound.
// `TestUnitContractTable` already checks the combined bound's arithmetic;
// this checks that the combined bound is actually wide enough for both
// `LookbackFold` and `AnchorFold` to agree between full and bounded data —
// the gap a bound-arithmetic-only check can't see (e.g. a wrong precedence
// or double-counting between the two terms could still leave one mode's own
// read outside the bound even while the numbers "look" additive).
void TestCombinedModeInvariant() {
	const unsigned kMode = LOOKBACK | ANCHOR;
	std::mt19937_64 rng(0xC0DEC0DE);
	for (int trial = 0; trial < kTrialsPerMode; trial++) {
		RandomTrial rt = MakeRandomTrial(rng, kMode);
		ScanBounds b = scan_bounds(rt.grid, rt.window, rt.lookback, kMode);

		bool any_excluded = false;
		for (const auto &s : rt.samples) {
			if (s.t < b.lower || s.t > b.upper) {
				any_excluded = true;
			}
		}
		Check(any_excluded, "LOOKBACK|ANCHOR: at least one sample must lie outside the scan bound");

		std::vector<Sample> bounded = Slice(rt.samples, b.lower, b.upper);
		for (int64_t i = 0; i < rt.grid.count(); i++) {
			int64_t anchor = rt.grid.at(i);
			Check(CombinedLookbackAnchorFold(anchor, rt.window, rt.lookback, rt.samples) ==
			          CombinedLookbackAnchorFold(anchor, rt.window, rt.lookback, bounded),
			      "LOOKBACK|ANCHOR: the combined fold over the full data must equal the fold over only the bounded "
			      "data");
		}
	}
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
	TestCombinedModeInvariant();
	TestDroppedLowerBoundTermMutant();

	if (g_failures > 0) {
		std::fprintf(stderr, "scan_bounds_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("scan_bounds_test: PASS\n");
	return 0;
}
