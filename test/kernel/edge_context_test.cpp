// edge_context_test.cpp — the L1a direct test for
// `src/kernel/edge_context.hpp` (docs/testing/layers.md's L1a row: "every
// Tier 0-5 primitive has its own translation unit, its own table-driven
// tests ... exercised directly"). Hand-rolled `main()`, no test framework,
// compiled and run with a bare `g++ -std=c++17` by
// `scripts/hygiene/kernel-primitive-tests.mjs` — the same
// dependency-free-TU pattern `window_walk_test.cpp` established.
//
// Structure follows `docs/testing/primitives.md`'s Tier 3 `edge_context`
// row: the five named unit-contract cases — including this issue's own
// acceptance criterion, the anchor-at-exactly-`t − w` row — the two
// invariants (`before.t <= anchor - width < first_in_window.t`;
// `after.t > anchor`) checked against a brute-force oracle over many
// randomized trials, and the named must-die mutant ("Off-by-one selecting
// the sample *in* the window as 'before'").
#include "kernel/edge_context.hpp"
#include "kernel/sample_buffer.hpp"
#include "kernel/window.hpp"

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
		std::fprintf(stderr, "edge_context_test: FAIL — %s\n", what);
		g_failures++;
	}
}

// The independent oracle: a brute-force scan for the last sample with
// `t <= anchor - width` and the first with `t > anchor`, spelled a second
// time with plain `__int128_t` arithmetic over an unsorted scan (a
// different code path from `edge_context.hpp`'s `std::upper_bound` over
// sorted storage).
EdgeContext BruteForceEdgeContext(const std::vector<Sample> &samples, int64_t anchor, int64_t width) {
	__int128_t threshold = static_cast<__int128_t>(anchor) - static_cast<__int128_t>(width);
	bool has_before = false;
	Sample before {0, 0.0};
	int64_t before_t = 0;
	bool has_after = false;
	Sample after {0, 0.0};
	int64_t after_t = 0;

	for (const Sample &s : samples) {
		if (static_cast<__int128_t>(s.t) <= threshold) {
			if (!has_before || s.t > before_t) {
				has_before = true;
				before = s;
				before_t = s.t;
			}
		}
		if (s.t > anchor) {
			if (!has_after || s.t < after_t) {
				has_after = true;
				after = s;
				after_t = s.t;
			}
		}
	}
	return EdgeContext {has_before, before, has_after, after};
}

// The five named unit-contract cases, each on a small explicit fixture.
void TestNamedUnitCases() {
	// No sample before.
	{
		std::vector<Sample> samples = {{95, 1.0}, {150, 2.0}};
		EdgeContext ec = edge_context(samples.data(), samples.size(), 100, 50); // threshold = 50
		Check(!ec.has_before, "no sample before: nothing at or before t - w = 50");
		Check(ec.has_after && ec.after.t == 150, "no sample before: the after sample is still found");
	}

	// One before at various gaps: exactly at the threshold, one microsecond
	// before it, and far before it.
	{
		std::vector<Sample> exactly = {{50, 1.0}};
		EdgeContext ec = edge_context(exactly.data(), exactly.size(), 100, 50); // threshold = 50
		Check(ec.has_before && ec.before.t == 50, "one before, exactly at the threshold (t == anchor - width)");

		std::vector<Sample> one_before = {{49, 1.0}};
		ec = edge_context(one_before.data(), one_before.size(), 100, 50);
		Check(ec.has_before && ec.before.t == 49, "one before, one microsecond before the threshold");

		std::vector<Sample> far_before = {{-1000, 1.0}};
		ec = edge_context(far_before.data(), far_before.size(), 100, 50);
		Check(ec.has_before && ec.before.t == -1000, "one before, far before the threshold");
	}

	// None after.
	{
		std::vector<Sample> samples = {{10, 1.0}, {50, 2.0}};
		EdgeContext ec = edge_context(samples.data(), samples.size(), 100, 50);
		Check(!ec.has_after, "none after: nothing past anchor 100");
		Check(ec.has_before && ec.before.t == 50, "none after: the before sample is still found");
	}

	// One after.
	{
		std::vector<Sample> samples = {{101, 1.0}};
		EdgeContext ec = edge_context(samples.data(), samples.size(), 100, 50);
		Check(ec.has_after && ec.after.t == 101, "one after: the first sample past anchor");
	}

	// INSIDE returns nothing: the primary window (anchor - width, anchor]
	// is empty of samples, but edge_context still finds before/after —
	// edge_context never looks inside the window at all, so an empty
	// INSIDE-window read has no bearing on it.
	{
		std::vector<Sample> samples = {{40, 1.0}, {110, 2.0}}; // neither is in (50, 100]
		Window w {100, 50};
		Check(!w.contains(40) && !w.contains(110), "sanity: INSIDE's own window admits neither sample");
		EdgeContext ec = edge_context(samples.data(), samples.size(), 100, 50);
		Check(ec.has_before && ec.before.t == 40, "INSIDE returns nothing: before is still found");
		Check(ec.has_after && ec.after.t == 110, "INSIDE returns nothing: after is still found");
	}
}

// This issue's own acceptance criterion: the anchor-at-exactly-`t - w` row.
// A sample sitting exactly on the threshold is `before` (the bound itself
// is inclusive, deliberately unlike `Window.contains`'s left-open interval
// — this is *not* the windowed inequality, it is edge_context's own
// single-sided "at or before" rule), and a sample one tick past it is
// `first_in_window`, never `before`.
void TestAnchorAtExactlyTMinusWRow() {
	const int64_t anchor = 1000, width = 200; // threshold = 800
	std::vector<Sample> samples = {{800, 1.0}, {801, 2.0}};
	EdgeContext ec = edge_context(samples.data(), samples.size(), anchor, width);
	Check(ec.has_before && ec.before.t == 800, "anchor-at-exactly-t-w: t == anchor - width is `before`");

	Window w {anchor, width};
	Check(w.contains(801), "sanity: t == anchor - width + 1 is the first sample the window admits");
	Check(ec.before.t <= anchor - width, "invariant: before.t <= anchor - width");
	Check(anchor - width < 801, "invariant: anchor - width < first_in_window.t");
}

// The invariants — `before.t <= anchor - width < first_in_window.t` (when a
// `before` and an in-window sample both exist) and `after.t > anchor` (when
// an `after` exists) — checked against the brute-force oracle over many
// randomized trials.
void TestRandomizedInvariants() {
	std::mt19937_64 rng(0xEDDE);
	std::uniform_int_distribution<int64_t> anchor_dist(-2000, 2000);
	std::uniform_int_distribution<int64_t> width_dist(0, 500);
	std::uniform_int_distribution<int64_t> sample_count_dist(0, 15);
	std::uniform_int_distribution<int64_t> t_dist(-3000, 3000);
	std::uniform_int_distribution<int64_t> value_dist(-1000, 1000);

	const int kTrials = 2000;
	for (int trial = 0; trial < kTrials; trial++) {
		int64_t anchor = anchor_dist(rng);
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

		EdgeContext real = edge_context(samples.data(), samples.size(), anchor, width);
		EdgeContext oracle = BruteForceEdgeContext(samples, anchor, width);

		Check(real.has_before == oracle.has_before, "randomized: has_before agrees with the brute-force oracle");
		if (real.has_before && oracle.has_before) {
			Check(real.before.t == oracle.before.t, "randomized: before.t agrees with the brute-force oracle");
			Check(real.before.t <= anchor - width, "invariant: before.t <= anchor - width");
		}
		Check(real.has_after == oracle.has_after, "randomized: has_after agrees with the brute-force oracle");
		if (real.has_after && oracle.has_after) {
			Check(real.after.t == oracle.after.t, "randomized: after.t agrees with the brute-force oracle");
			Check(real.after.t > anchor, "invariant: after.t > anchor");
		}

		Window w {anchor, width};
		for (const Sample &s : samples) {
			if (w.contains(s.t) && real.has_before) {
				Check(real.before.t <= anchor - width && anchor - width < s.t,
				      "invariant: before.t <= anchor - width < first_in_window.t");
			}
		}
	}
}

// Must-die mutant: "Off-by-one selecting the sample *in* the window as
// 'before'" — computing `before` against `anchor` instead of
// `anchor - width`, so a sample that is actually inside the window (or
// past it) gets reported as `before`.
EdgeContext MutantBeforeUsesAnchor(const Sample *data, std::size_t n, int64_t anchor, int64_t /*width*/) {
	const Sample *begin = data;
	const Sample *end = data + n;
	// mutated: thresholds on `anchor`, not `anchor - width`.
	const Sample *before_it = std::upper_bound(begin, end, anchor, [](int64_t a, const Sample &s) { return a < s.t; });
	bool has_before = before_it != begin;
	Sample before = has_before ? *(before_it - 1) : Sample {0, 0.0};
	return EdgeContext {has_before, before, false, Sample {0, 0.0}};
}

void TestMutantBeforeUsesAnchorDies() {
	// Window (90, 100]: t = 95 is *inside* the window, not before it.
	std::vector<Sample> samples = {{95, 1.0}};
	EdgeContext real = edge_context(samples.data(), samples.size(), 100, 10);
	EdgeContext mutant = MutantBeforeUsesAnchor(samples.data(), samples.size(), 100, 10);

	Check(!real.has_before, "must-die (before uses anchor): the real function finds no sample at or before t - w = 90");
	Check(mutant.has_before && mutant.before.t == 95,
	      "must-die (before uses anchor): the mutant wrongly reports the in-window sample (t=95) as `before`");
}

} // namespace

int main() {
	TestNamedUnitCases();
	TestAnchorAtExactlyTMinusWRow();
	TestRandomizedInvariants();
	TestMutantBeforeUsesAnchorDies();

	if (g_failures > 0) {
		std::fprintf(stderr, "edge_context_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("edge_context_test: PASS\n");
	return 0;
}
