// window_walk_test.cpp — the L1a direct test for
// `src/kernel/window_walk.hpp` (docs/testing/layers.md's L1a row: "every
// Tier 0-5 primitive has its own translation unit, its own table-driven
// tests ... exercised directly"). Hand-rolled `main()`, no test framework,
// compiled and run with a bare `g++ -std=c++17` by
// `scripts/hygiene/kernel-primitive-tests.mjs` — the same
// dependency-free-TU pattern `scan_bounds_test.cpp` established.
//
// Structure follows `docs/testing/primitives.md`'s Tier 3 `window_walk` row:
// the seven named unit-contract cases, the two invariants — brute-force
// membership equivalence against `Window.contains`, and `lo`/`hi` monotone
// non-decreasing across the grid (this issue's own acceptance criterion,
// "Brute-force equivalence and monotone-pointer properties") — checked over
// many randomized trials, and the two named must-die mutants ("Pointer
// advance conditions" and "monotonicity shortcuts that skip a sample").
#include "../../src/kernel/grid.hpp"
#include "../../src/kernel/sample_buffer.hpp"
#include "../../src/kernel/window.hpp"
#include "../../src/kernel/window_walk.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using namespace chronoduck;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		std::fprintf(stderr, "window_walk_test: FAIL — %s\n", what);
		g_failures++;
	}
}

// The independent oracle this row names: a brute-force per-point scan with
// `Window.contains`, checked as a membership predicate over every sample
// index rather than as an exact `(lo, hi)` pair — an empty window is
// correctly represented by `lo == hi` at *any* consistent pointer position
// (the two-pointer walk's own freedom, not part of the primitive's
// contract), so membership is what "contains exactly the samples
// `Window.contains` admits" actually means.
void CheckMembership(const std::vector<Sample> &samples, const Grid &grid, int64_t width,
                     const std::vector<WindowRange> &ranges, const char *label) {
	for (int64_t i = 0; i < grid.count(); i++) {
		Window w {grid.at(i), width};
		const WindowRange &r = ranges[static_cast<std::size_t>(i)];
		for (std::size_t j = 0; j < samples.size(); j++) {
			bool in_range = j >= r.lo && j < r.hi;
			bool in_window = w.contains(samples[j].t);
			if (in_range != in_window) {
				std::fprintf(stderr,
				             "window_walk_test: FAIL — %s: grid point %lld sample %zu (t=%lld) in_range=%d "
				             "in_window=%d\n",
				             label, static_cast<long long>(i), j, static_cast<long long>(samples[j].t), in_range,
				             in_window);
				g_failures++;
			}
		}
	}
}

// `lo`/`hi` non-decreasing across the whole grid, and `lo <= hi` at every
// point — the walk's own monotone-pointer property.
void CheckMonotone(const std::vector<WindowRange> &ranges, const char *label) {
	for (std::size_t i = 0; i < ranges.size(); i++) {
		Check(ranges[i].lo <= ranges[i].hi, label);
		if (i > 0) {
			Check(ranges[i].lo >= ranges[i - 1].lo, label);
			Check(ranges[i].hi >= ranges[i - 1].hi, label);
		}
	}
}

// The seven named unit-contract cases, each on a small explicit fixture.
void TestNamedUnitCases() {
	// Empty buffer.
	{
		std::vector<Sample> samples;
		Grid grid(0, 200, 100);
		auto ranges = window_walk(samples.data(), samples.size(), grid, 50);
		Check(ranges.size() == 3, "empty buffer: one range per grid point");
		for (const auto &r : ranges) {
			Check(r.lo == 0 && r.hi == 0, "empty buffer: every range is {0, 0}");
		}
	}

	// One sample.
	{
		std::vector<Sample> samples = {{95, 1.0}};
		Grid grid(0, 200, 100); // anchors 0, 100, 200
		auto ranges = window_walk(samples.data(), samples.size(), grid, 50);
		CheckMembership(samples, grid, 50, ranges, "one sample");
		// Sample at 95 falls in (50, 100] (anchor 100) only.
		Check(ranges[0].lo == 0 && ranges[0].hi == 0, "one sample: outside window at anchor 0");
		Check(ranges[1].lo == 0 && ranges[1].hi == 1, "one sample: inside window at anchor 100");
		Check(ranges[2].lo == 1 && ranges[2].hi == 1, "one sample: past window at anchor 200");
	}

	// Samples all before the grid.
	{
		std::vector<Sample> samples = {{-500, 1.0}, {-400, 2.0}, {-300, 3.0}};
		Grid grid(0, 200, 100);
		auto ranges = window_walk(samples.data(), samples.size(), grid, 50);
		CheckMembership(samples, grid, 50, ranges, "all before the grid");
		for (const auto &r : ranges) {
			Check(r.hi - r.lo == 0, "all before the grid: every window is empty");
		}
	}

	// Samples all after the grid (beyond even the last window).
	{
		std::vector<Sample> samples = {{1000, 1.0}, {1100, 2.0}};
		Grid grid(0, 200, 100);
		auto ranges = window_walk(samples.data(), samples.size(), grid, 50);
		CheckMembership(samples, grid, 50, ranges, "all after the grid");
		for (const auto &r : ranges) {
			Check(r.lo == 0 && r.hi == 0, "all after the grid: every window is empty, pointers stay at 0");
		}
	}

	// Exactly on every edge: one sample at anchor - width (excluded), one at
	// anchor (included).
	{
		std::vector<Sample> samples = {{50, 1.0}, {100, 2.0}};
		Grid grid(100, 100, 1); // single grid point, anchor 100
		auto ranges = window_walk(samples.data(), samples.size(), grid, 50);
		CheckMembership(samples, grid, 50, ranges, "exactly on every edge");
		Check(ranges[0].lo == 1 && ranges[0].hi == 2,
		      "exactly on every edge: t == anchor - width excluded, t == anchor included");
	}

	// Window smaller than sample spacing: every other grid point sees an
	// empty window.
	{
		std::vector<Sample> samples = {{0, 1.0}, {100, 2.0}, {200, 3.0}};
		Grid grid(0, 200, 100);
		auto ranges = window_walk(samples.data(), samples.size(), grid, 5); // width << spacing
		CheckMembership(samples, grid, 5, ranges, "window smaller than sample spacing");
		Check(ranges[0].hi - ranges[0].lo == 1, "narrow window: anchor 0 catches only t=0");
		Check(ranges[1].hi - ranges[1].lo == 1, "narrow window: anchor 100 catches only t=100");
		Check(ranges[2].hi - ranges[2].lo == 1, "narrow window: anchor 200 catches only t=200");
	}

	// Window spanning the whole buffer.
	{
		std::vector<Sample> samples = {{0, 1.0}, {50, 2.0}, {100, 3.0}, {150, 4.0}, {200, 5.0}};
		Grid grid(200, 200, 1); // single grid point, anchor 200
		auto ranges = window_walk(samples.data(), samples.size(), grid, 1000000);
		CheckMembership(samples, grid, 1000000, ranges, "window spanning the whole buffer");
		Check(ranges[0].lo == 0 && ranges[0].hi == 5, "window spanning the whole buffer: every sample admitted");
	}
}

// The invariant: for every grid point, `[lo, hi)` contains exactly the
// samples `Window.contains` admits, and `lo`/`hi` are monotone
// non-decreasing across the grid — over many randomized trials.
void TestRandomizedInvariants() {
	std::mt19937_64 rng(0x1AB121E);
	std::uniform_int_distribution<int64_t> start_dist(-2000, 2000);
	std::uniform_int_distribution<int64_t> step_dist(1, 40);
	std::uniform_int_distribution<int64_t> points_dist(1, 12);
	std::uniform_int_distribution<int64_t> width_dist(0, 150);
	std::uniform_int_distribution<int64_t> sample_count_dist(0, 20);
	std::uniform_int_distribution<int64_t> t_dist(-3000, 3000);
	std::uniform_int_distribution<int64_t> value_dist(-1000, 1000);

	const int kTrials = 500;
	for (int trial = 0; trial < kTrials; trial++) {
		int64_t start = start_dist(rng);
		int64_t step = step_dist(rng);
		int64_t points = points_dist(rng);
		int64_t end = start + step * (points - 1);
		Grid grid(start, end, step);
		int64_t width = width_dist(rng);

		std::vector<Sample> samples;
		int64_t n = sample_count_dist(rng);
		for (int64_t i = 0; i < n; i++) {
			samples.push_back({t_dist(rng), static_cast<double>(value_dist(rng))});
		}
		std::sort(samples.begin(), samples.end(), [](const Sample &a, const Sample &b) { return a.t < b.t; });
		samples.erase(
		    std::unique(samples.begin(), samples.end(), [](const Sample &a, const Sample &b) { return a.t == b.t; }),
		    samples.end());

		auto ranges = window_walk(samples.data(), samples.size(), grid, width);
		Check(ranges.size() == static_cast<std::size_t>(grid.count()), "one range per grid point");
		CheckMembership(samples, grid, width, ranges, "randomized trial");
		CheckMonotone(ranges, "randomized trial: lo/hi monotone non-decreasing and lo <= hi");
	}
}

// Must-die mutant 1 ("Pointer advance conditions"): `hi`'s `<=` weakened to
// `<`, excluding a sample exactly at the anchor — Window.contains's own
// right-closed edge.
std::vector<WindowRange> MutantHiExclusive(const Sample *data, std::size_t n, const Grid &grid, int64_t width) {
	std::vector<WindowRange> ranges;
	std::size_t lo = 0, hi = 0;
	for (int64_t i = 0; i < grid.count(); i++) {
		int64_t anchor = grid.at(i);
		Window w {anchor, width};
		while (lo < n && !w.contains(data[lo].t) && data[lo].t <= anchor) {
			lo++;
		}
		while (hi < n && data[hi].t < anchor) { // mutated: <= -> <
			hi++;
		}
		ranges.push_back({lo, hi});
	}
	return ranges;
}

void TestMutantHiExclusiveDies() {
	std::vector<Sample> samples = {{100, 1.0}};
	Grid grid(100, 100, 1); // anchor 100 == the sample's own t
	auto real = window_walk(samples.data(), samples.size(), grid, 50);
	auto mutant = MutantHiExclusive(samples.data(), samples.size(), grid, 50);
	Check(real[0].hi == 1, "must-die (hi exclusive): the real walk admits the sample exactly at the anchor");
	Check(mutant[0].hi == 0,
	      "must-die (hi exclusive): the mutant excludes the sample exactly at the anchor, disagreeing with the real "
	      "walk");
}

// Must-die mutant 2 ("monotonicity shortcuts that skip a sample"): `lo`
// drops its `data[lo].t <= anchor` guard, advancing past any sample that
// merely fails `Window.contains` right now — including a *future* sample
// (`t > anchor`) that a later, wider window is supposed to still admit.
std::vector<WindowRange> MutantLoNoUpperGuard(const Sample *data, std::size_t n, const Grid &grid, int64_t width) {
	std::vector<WindowRange> ranges;
	std::size_t lo = 0, hi = 0;
	for (int64_t i = 0; i < grid.count(); i++) {
		int64_t anchor = grid.at(i);
		Window w {anchor, width};
		while (lo < n && !w.contains(data[lo].t)) { // mutated: dropped "&& data[lo].t <= anchor"
			lo++;
		}
		while (hi < n && data[hi].t <= anchor) {
			hi++;
		}
		ranges.push_back({lo, hi});
	}
	return ranges;
}

void TestMutantLoNoUpperGuardDies() {
	// A two-point grid at a single fixed width (window_walk's own
	// signature — one width for the whole walk): at anchor 0, window
	// (-60, 0] excludes the sample at t=50 (t > anchor: it hasn't happened
	// "yet" relative to this window, not that it's too old); at anchor 100,
	// window (40, 100] admits it. The real walk's `lo` guard keeps it at 0
	// through the first, empty window so the second window still finds it;
	// the mutant's `lo` races past it during the first window and never
	// finds it again.
	std::vector<Sample> samples = {{50, 1.0}};
	Grid grid(0, 100, 100); // anchors 0, 100
	int64_t width = 60;
	auto real = window_walk(samples.data(), samples.size(), grid, width);
	auto mutant = MutantLoNoUpperGuard(samples.data(), samples.size(), grid, width);

	Window w1 {100, width};
	Check(w1.contains(50), "sanity: Window.contains admits t=50 at anchor 100, width 60 -> (40, 100]");
	Check(real[1].lo <= 0 && 0 < real[1].hi,
	      "must-die (lo no upper guard): the real walk still finds sample index 0 at anchor 100");
	Check(!(mutant[1].lo <= 0 && 0 < mutant[1].hi),
	      "must-die (lo no upper guard): the mutant already discarded sample index 0 while anchor was still 0, "
	      "and never finds it again at anchor 100");
}

} // namespace

int main() {
	TestNamedUnitCases();
	TestRandomizedInvariants();
	TestMutantHiExclusiveDies();
	TestMutantLoNoUpperGuardDies();

	if (g_failures > 0) {
		std::fprintf(stderr, "window_walk_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("window_walk_test: PASS\n");
	return 0;
}
