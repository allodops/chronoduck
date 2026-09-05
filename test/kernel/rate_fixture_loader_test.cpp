// rate_fixture_loader_test.cpp — the L1a direct test for
// `rate_fixture_eval.hpp`'s `EvaluateRate` (docs/testing/layers.md's L1a
// row: "every ... primitive has its own translation unit, its own
// table-driven tests ... exercised directly"). Hand-rolled `main()`, no test
// framework, compiled and run with a bare `g++ -std=c++17` by
// `scripts/hygiene/kernel-primitive-tests.mjs` — the same dependency-free-TU
// pattern `extrapolate_test.cpp` established.
//
// Every case here is the same scenario as one of `test/fixtures/rate/*.yaml`
// (this issue's own hand-derived fixtures), checked directly in C++ against
// `EvaluateRate` — independent of `scripts/kernel-fixture-loader.mjs`'s YAML
// parsing and wire-format marshalling, so a bug in the composition itself is
// caught here even if the loader script were never run.
#include "src/kernel/comparator.hpp"
#include "rate_fixture_eval.hpp"

#include <cstdio>

using namespace chronoduck;
using namespace chronoduck::fixtures;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		std::fprintf(stderr, "rate_fixture_loader_test: FAIL — %s\n", what);
		g_failures++;
	}
}

RawSample R(int64_t t, double v) {
	return RawSample {t, v, false, 0};
}

RawSample RS(int64_t t, double v, int64_t st) {
	return RawSample {t, v, true, st};
}

// rate/dup-duplicate-timestamp.yaml: the 50-vs-37.5 duplicate-timestamp
// fixture. `EvaluateRate` always goes through the real `SampleBuffer::sort_dedup`,
// so this test only ever sees the correct (50) answer — 37.5 is the
// documented wrong value a non-deduping implementation would produce
// (recorded in the fixture's own `wrong:` block, not reproduced by a second
// code path here).
void TestDupDuplicateTimestamp() {
	std::vector<RawSample> samples = {R(10, 1000), R(10, 700), R(70, 4000)};
	Grid grid {112, 112, 1};
	auto out = EvaluateRate(samples, grid, 108);
	Check(out.size() == 1, "dup: one grid point");
	Check(out[0].has_value, "dup: has a value");
	Check(equal_values(out[0].value, 50.0, out[0].scale), "dup: deduped rate == 50");
}

// rate/reset-midwindow.yaml: one reset mid-window.
void TestResetMidwindow() {
	std::vector<RawSample> samples = {R(5, 10), R(65, 20), R(125, 5), R(185, 15)};
	Grid grid {205, 205, 1};
	auto out = EvaluateRate(samples, grid, 205);
	Check(out[0].has_value, "reset: has a value");
	Check(equal_values(out[0].value, 25.0 / 180.0, out[0].scale), "reset: rate == 25/180");
}

// rate/edge-boundary.yaml: left-open, right-closed window.
void TestEdgeBoundary() {
	std::vector<RawSample> samples = {R(100, 999), R(101, 10), R(200, 40)};
	Grid grid {200, 200, 1};
	auto out = EvaluateRate(samples, grid, 100);
	Check(out[0].has_value, "edge: has a value (decoy at t=100 excluded)");
	Check(equal_values(out[0].value, 30.0 / 99.0, out[0].scale), "edge: rate == 30/99, the decoy never folded in");
}

// rate/threshold-single-sample-without-st.yaml: one sample, no `st` -> null.
void TestThresholdSingleSampleWithoutSt() {
	std::vector<RawSample> samples = {R(50, 10)};
	Grid grid {100, 100, 1};
	auto out = EvaluateRate(samples, grid, 100);
	Check(!out[0].has_value, "threshold: one sample, no st -> no value");
}

// rate/threshold-single-sample-with-st.yaml: one sample, `st` strictly
// inside the window -> a value. This issue's own acceptance criterion, both
// halves.
void TestThresholdSingleSampleWithSt() {
	std::vector<RawSample> samples = {RS(100, 5, 80)};
	Grid grid {110, 110, 1};
	auto out = EvaluateRate(samples, grid, 60);
	Check(out[0].has_value, "threshold: one sample with a bound, in-window st -> has a value");
	Check(equal_values(out[0].value, 0.125, out[0].scale), "threshold: rate == 0.125");
}

// rate/clamp-zero-crossing.yaml: the durationToZero clamp.
void TestClampZeroCrossing() {
	std::vector<RawSample> samples = {R(100, 1), R(110, 50), R(120, 101)};
	Grid grid {125, 125, 1};
	auto out = EvaluateRate(samples, grid, 125);
	Check(out[0].has_value, "clamp: has a value");
	Check(equal_values(out[0].value, 1.008, out[0].scale), "clamp: rate == 1.008 (clamp engaged)");
}

// rate/start-ts-reset-no-value-drop.yaml: st_reset fires with no value drop.
void TestStartTsResetNoValueDrop() {
	std::vector<RawSample> samples = {R(10, 10), RS(20, 15, 25), R(30, 20)};
	Grid grid {35, 35, 1};
	auto out = EvaluateRate(samples, grid, 30);
	Check(out[0].has_value, "start-ts: has a value");
	Check(equal_values(out[0].value, 1.0, out[0].scale), "start-ts: rate == 1.0 (st_reset fired, no value drop)");
}

// rate/cadence-window-smaller-than-scrape.yaml: every point null.
void TestCadenceWindowSmallerThanScrape() {
	std::vector<RawSample> samples = {R(30, 10), R(60, 20), R(90, 35)};
	Grid grid {30, 90, 30};
	auto out = EvaluateRate(samples, grid, 20);
	Check(out.size() == 3, "cadence: three grid points");
	for (std::size_t i = 0; i < out.size(); i++) {
		Check(!out[i].has_value, "cadence: window narrower than the scrape interval never brackets two samples");
	}
}

// rate/sweep-eval-instant-offsets.yaml: only anchors in [1060, 1080) bracket
// both samples; everything else, including the far end, is null.
void TestSweepEvalInstantOffsets() {
	std::vector<RawSample> samples = {R(1000, 100), R(1060, 180)};
	Grid grid {1000, 1300, 15};
	auto out = EvaluateRate(samples, grid, 80);
	Check(out.size() == 21, "sweep: 21 grid points");
	for (std::size_t i = 0; i < out.size(); i++) {
		bool expect_value = (i == 4 || i == 5);
		Check(out[i].has_value == expect_value, "sweep: only the two anchors bracketing both samples have a value");
		if (expect_value) {
			Check(equal_values(out[i].value, 80.0 / 60.0, out[i].scale), "sweep: rate == 80/60 on both anchors");
		}
	}
}

} // namespace

int main() {
	TestDupDuplicateTimestamp();
	TestResetMidwindow();
	TestEdgeBoundary();
	TestThresholdSingleSampleWithoutSt();
	TestThresholdSingleSampleWithSt();
	TestClampZeroCrossing();
	TestStartTsResetNoValueDrop();
	TestCadenceWindowSmallerThanScrape();
	TestSweepEvalInstantOffsets();

	if (g_failures > 0) {
		std::fprintf(stderr, "rate_fixture_loader_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("rate_fixture_loader_test: PASS\n");
	return 0;
}
