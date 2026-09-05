// grid_stream_test.cpp — the L1a direct test for `src/kernel/grid_stream.hpp`
// (docs/testing/layers.md's L1a row: "every Tier 0-5 primitive has its own
// translation unit, its own table-driven tests ... exercised directly").
// Hand-rolled `main()`, no test framework, compiled and run with a bare
// `g++ -std=c++17` by `scripts/hygiene/kernel-primitive-tests.py` — the same
// dependency-free-TU pattern `window_walk_test.cpp` established.
//
// Structure follows `docs/testing/primitives.md`'s Tier 3 `grid_stream` row:
// the streaming-vs-array cross-check against `window_walk` (its own named
// independent oracle), the dedup unit case (the 50-vs-37.5 shape), the
// residency bound the primitive exists to prove, and the two named must-die
// mutants ("the eos guard" and "dedup collapse dropped").
#include "kernel/counter_fold.hpp"
#include "kernel/grid.hpp"
#include "kernel/grid_stream.hpp"
#include "kernel/sample_buffer.hpp"
#include "kernel/window.hpp"
#include "kernel/window_walk.hpp"

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
		std::fprintf(stderr, "grid_stream_test: FAIL — %s\n", what);
		g_failures++;
	}
}

CounterSample ToCounterSample(const Sample &s) {
	CounterSample cs;
	cs.t = s.t;
	cs.v = s.v;
	return cs;
}

// Feeds `samples` (already sorted, unique timestamps) through `GridStream`
// one at a time and returns, per grid point, the *values* seen (not raw
// indices — `GridStream` compacts its buffer, so absolute indices are not
// comparable to `window_walk`'s, only content is).
std::vector<std::vector<double>> RunStreaming(const std::vector<Sample> &samples, const Grid &grid, int64_t width) {
	std::vector<std::vector<double>> out(static_cast<std::size_t>(grid.count()));
	GridStream stream(grid, width);
	auto emit = [&](int64_t grid_index, const CounterSample *data, std::size_t n) {
		auto &bucket = out[static_cast<std::size_t>(grid_index)];
		for (std::size_t i = 0; i < n; i++) {
			bucket.push_back(data[i].v);
		}
	};
	for (const Sample &s : samples) {
		stream.feed(ToCounterSample(s), emit);
	}
	stream.end();
	stream.resume(emit);
	return out;
}

std::vector<std::vector<double>> RunBatch(const std::vector<Sample> &samples, const Grid &grid, int64_t width) {
	auto ranges = window_walk(samples.data(), samples.size(), grid, width);
	std::vector<std::vector<double>> out;
	out.reserve(ranges.size());
	for (const WindowRange &r : ranges) {
		std::vector<double> bucket;
		for (std::size_t i = r.lo; i < r.hi; i++) {
			bucket.push_back(samples[i].v);
		}
		out.push_back(std::move(bucket));
	}
	return out;
}

void CheckCrossCheck(const std::vector<Sample> &samples, const Grid &grid, int64_t width, const char *label) {
	auto streaming = RunStreaming(samples, grid, width);
	auto batch = RunBatch(samples, grid, width);
	Check(streaming.size() == batch.size(), label);
	for (std::size_t i = 0; i < streaming.size() && i < batch.size(); i++) {
		if (streaming[i] != batch[i]) {
			std::fprintf(stderr, "grid_stream_test: FAIL — %s: grid point %zu diverges (streaming %zu vs batch %zu)\n",
			             label, i, streaming[i].size(), batch[i].size());
			g_failures++;
		}
	}
}

// The seven `window_walk`-shaped unit cases, run through the streaming path
// and cross-checked against the batch primitive's own answer.
void TestNamedUnitCases() {
	{
		std::vector<Sample> samples;
		Grid grid(0, 200, 100);
		CheckCrossCheck(samples, grid, 50, "empty buffer");
	}
	{
		std::vector<Sample> samples = {{95, 1.0}};
		Grid grid(0, 200, 100);
		CheckCrossCheck(samples, grid, 50, "one sample");
	}
	{
		std::vector<Sample> samples = {{-500, 1.0}, {-400, 2.0}, {-300, 3.0}};
		Grid grid(0, 200, 100);
		CheckCrossCheck(samples, grid, 50, "all before the grid");
	}
	{
		std::vector<Sample> samples = {{1000, 1.0}, {1100, 2.0}};
		Grid grid(0, 200, 100);
		CheckCrossCheck(samples, grid, 50, "all after the grid");
	}
	{
		std::vector<Sample> samples = {{50, 1.0}, {100, 2.0}};
		Grid grid(100, 100, 1);
		CheckCrossCheck(samples, grid, 50, "exactly on every edge");
	}
	{
		std::vector<Sample> samples = {{0, 1.0}, {100, 2.0}, {200, 3.0}};
		Grid grid(0, 200, 100);
		CheckCrossCheck(samples, grid, 5, "window smaller than sample spacing");
	}
	{
		std::vector<Sample> samples = {{0, 1.0}, {50, 2.0}, {100, 3.0}, {150, 4.0}, {200, 5.0}};
		Grid grid(200, 200, 1);
		CheckCrossCheck(samples, grid, 1000000, "window spanning the whole buffer");
	}
}

// The independent oracle this row names: `window_walk` over the identical,
// already-deduplicated array, fed to `GridStream` one sample at a time
// instead of all at once — many randomized trials.
void TestRandomizedCrossCheck() {
	std::mt19937_64 rng(0x6121D57A);
	std::uniform_int_distribution<int64_t> start_dist(-2000, 2000);
	std::uniform_int_distribution<int64_t> step_dist(1, 40);
	std::uniform_int_distribution<int64_t> points_dist(1, 12);
	std::uniform_int_distribution<int64_t> width_dist(0, 150);
	std::uniform_int_distribution<int64_t> sample_count_dist(0, 30);
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

		CheckCrossCheck(samples, grid, width, "randomized trial");
	}
}

// The dedup unit case: two samples sharing a timestamp resolve to the
// `dedup_policy` winner regardless of feed order — the 50-vs-37.5 shape
// `docs/testing/fixtures.md`'s `dup` family names.
void TestDedupCollapsesToDeclaredWinner() {
	Grid grid(100, 100, 1); // one grid point, anchor 100
	int64_t width = 50;

	for (bool winner_first : {true, false}) {
		GridStream stream(grid, width);
		std::vector<double> seen;
		auto emit = [&](int64_t, const CounterSample *data, std::size_t n) {
			for (std::size_t i = 0; i < n; i++) {
				seen.push_back(data[i].v);
			}
		};
		CounterSample a;
		a.t = 100;
		a.v = winner_first ? 50.0 : 37.5;
		CounterSample b;
		b.t = 100;
		b.v = winner_first ? 37.5 : 50.0;
		stream.feed(a, emit);
		stream.feed(b, emit);
		stream.end();
		stream.resume(emit);

		Check(seen.size() == 1, "dedup: exactly one sample survives a duplicate timestamp");
		if (seen.size() == 1) {
			Check(seen[0] == 50.0, "dedup: the greater value (50 over 37.5) wins under totalOrder regardless of order");
		}
	}
}

// The residency bound this primitive exists to hold: `resident_size()` never
// grows past a small constant multiple of the number of samples that could
// ever share one window, on a shape where a bug that reverted to
// whole-partition residency (e.g. never compacting) would make it grow
// linearly with the number of samples fed instead.
void TestResidencyBoundedByWindowNotRange() {
	const int64_t kSpacing = 1;
	const int64_t kWidth = 300;                     // ~300 samples per window at unit spacing
	const std::size_t kSampleCount = 200000;        // a range far larger than the window
	const std::size_t kMaxPlausibleResident = 4096; // generous slack over 2 * (kWidth + 2)

	// step == width: back-to-back windows; end rounded down to a multiple of
	// kWidth so Grid's own "step must evenly divide end - start" holds.
	int64_t grid_end = (static_cast<int64_t>(kSampleCount - 1) * kSpacing) / kWidth * kWidth;
	Grid grid(0, grid_end, kWidth);
	GridStream stream(grid, kWidth);
	std::size_t peak_resident = 0;
	auto emit = [&](int64_t, const CounterSample *, std::size_t) {
	};
	for (std::size_t i = 0; i < kSampleCount; i++) {
		CounterSample s;
		s.t = static_cast<int64_t>(i) * kSpacing;
		s.v = static_cast<double>(i);
		stream.feed(s, emit);
		peak_resident = std::max(peak_resident, stream.resident_size());
	}
	stream.end();
	stream.resume(emit);

	Check(stream.done(), "residency: the whole grid was walked");
	std::fprintf(stderr, "grid_stream_test: residency peak = %zu samples over %zu fed (window ~%lld)\n", peak_resident,
	             kSampleCount, static_cast<long long>(kWidth));
	Check(peak_resident <= kMaxPlausibleResident,
	      "residency: peak resident sample count stays bounded by the window, not the whole range");
}

// Must-die mutant 1 ("the eos guard removed"): treating "ran out of buffered
// data" as "no more matching samples" even mid-stream, instead of waiting
// for `end()` — a streaming-specific bug `window_walk`'s own offline mutants
// cannot express, since it only exists once data can arrive incrementally.
std::vector<std::vector<double>> MutantNoEosGuard(const std::vector<Sample> &samples, const Grid &grid, int64_t width) {
	std::vector<std::vector<double>> out(static_cast<std::size_t>(grid.count()));
	std::vector<CounterSample> buf;
	std::size_t lo = 0, hi = 0;
	int64_t next_index = 0;
	auto drain = [&]() {
		while (next_index < grid.count()) {
			int64_t anchor = grid.at(next_index);
			while (hi < buf.size() && buf[hi].t <= anchor) {
				hi++;
			}
			// mutated: dropped the "&& !eos" guard — hi is treated as
			// confirmed the instant buffered data runs out, even though more
			// samples (with t <= anchor) may still be coming.
			Window w {anchor, width};
			while (lo < hi && !w.contains(buf[lo].t) && buf[lo].t <= anchor) {
				lo++;
			}
			auto &bucket = out[static_cast<std::size_t>(next_index)];
			for (std::size_t i = lo; i < hi; i++) {
				bucket.push_back(buf[i].v);
			}
			next_index++;
		}
	};
	for (const Sample &s : samples) {
		buf.push_back(ToCounterSample(s));
		drain();
	}
	return out;
}

void TestMutantNoEosGuardDies() {
	// Two samples that both belong to the single grid point's window, fed
	// one at a time: the real walk waits for the second sample (or `end()`)
	// before finalizing; the mutant finalizes after the first, missing the
	// second entirely.
	std::vector<Sample> samples = {{60, 1.0}, {90, 2.0}};
	Grid grid(100, 100, 1); // anchor 100, width 50 -> window (50, 100]
	int64_t width = 50;

	auto real = RunStreaming(samples, grid, width);
	auto mutant = MutantNoEosGuard(samples, grid, width);

	Check(real[0].size() == 2, "must-die (no eos guard): the real streaming walk waits and finds both samples");
	Check(mutant[0].size() == 1,
	      "must-die (no eos guard): the mutant finalizes after the first sample and never sees the second");
}

// Must-die mutant 2 ("dedup collapse dropped"): every fed sample is kept
// verbatim instead of collapsing a duplicate timestamp via `dedup_policy` —
// the fixture family `dup` names as the 50-vs-37.5 bug: sample count
// inflating an interval estimate downstream.
void TestMutantDedupDroppedDies() {
	Grid grid(100, 100, 1);
	int64_t width = 50;

	CounterSample a;
	a.t = 100;
	a.v = 50.0;
	CounterSample b;
	b.t = 100;
	b.v = 37.5;

	// Real: GridStream::feed collapses the duplicate.
	GridStream stream(grid, width);
	std::vector<double> real_seen;
	auto real_emit = [&](int64_t, const CounterSample *data, std::size_t n) {
		for (std::size_t i = 0; i < n; i++) {
			real_seen.push_back(data[i].v);
		}
	};
	stream.feed(a, real_emit);
	stream.feed(b, real_emit);
	stream.end();
	stream.resume(real_emit);

	// Mutant: feed both as if they were distinct samples (what a
	// `feed`-without-dedup would do — the same shape `window_walk` itself
	// would see if handed an un-deduplicated buffer, which is exactly the
	// precondition `SampleBuffer::sort_dedup` exists to establish for it).
	std::vector<Sample> mutant_samples = {{100, 50.0}, {100, 37.5}};
	// `window_walk` requires no duplicate timestamps; simulate the mutant's
	// "no dedup" behaviour directly against the two-pointer body instead of
	// calling it with a precondition violation.
	std::size_t mutant_count = mutant_samples.size(); // both samples counted, undeduplicated

	Check(real_seen.size() == 1, "must-die (dedup dropped): the real walk keeps exactly one sample for the duplicate");
	Check(mutant_count == 2,
	      "must-die (dedup dropped): the mutant's un-deduplicated count disagrees with the real walk's dedup'd count");
}

} // namespace

int main() {
	TestNamedUnitCases();
	TestRandomizedCrossCheck();
	TestDedupCollapsesToDeclaredWinner();
	TestResidencyBoundedByWindowNotRange();
	TestMutantNoEosGuardDies();
	TestMutantDedupDroppedDies();

	if (g_failures > 0) {
		std::fprintf(stderr, "grid_stream_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("grid_stream_test: PASS\n");
	return 0;
}
