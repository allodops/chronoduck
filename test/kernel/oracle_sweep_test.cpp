// oracle_sweep_test.cpp — issue #42's own L3 driver (docs/testing/layers.md's
// L3 row): the ShapeID roster's deterministic worked examples, the property
// sweep over generated series x grids x windows, the eval-instant sweep
// `T in {0,15,30,60,90,120,180,300}`, and the ten metamorphic relations this
// issue scopes in (PART, PERM, SHIFT, SCALE, RESET, ST, DUP, EDGE, GRID,
// EMPTY), all checked against the REAL `ts_rate` composition — not a second
// re-implementation of it.
//
// The "x modes" axis of the property sweep is, today, a sweep of exactly one
// mode: `src/kernel/registry.def`'s `ts_rate` row claims `EXTRAPOLATE` only
// (that row's own comment: "the edge modes this issue's host glue actually
// wires up and fences"), so there is no second edge mode to compare against
// yet — MODE is this issue's own stated out-of-scope for exactly that
// reason. `test/oracle/shape_examples.hpp`'s `ShapeIdRoster()` and this
// file's generators both key off the registry, so a future issue that adds
// `ANCHOR`/`SMOOTH` extends the sweep by adding a shape and a generator leg,
// not by rewriting this file's structure.
//
// This file lives under `test/kernel/`, not `test/oracle/`: it is the one
// place the from-scratch world (`test/oracle/*.hpp`, fenced from `src/` by
// `scripts/hygiene/oracle-fence.mjs`) and the real kernel composition meet,
// exactly the role `test/kernel/rate_fixture_eval.hpp` already plays for L2
// (its own header comment: "used only by `rate_fixture_loader.cpp`'s CLI...,
// never included from `src/`"). Compiled and run with a bare
// `g++ -std=c++17` by `scripts/hygiene/kernel-primitive-tests.mjs`, the same
// dependency-free-TU pattern every other `test/kernel/*_test.cpp` follows —
// "provably executed by an unconditional, failure-propagating lane"
// (Article V.5) needs no new machinery here, since that scan already globs
// every `test/kernel/*_test.cpp`.
//
// `SHAPE PASS <id>` / `SHAPE FAIL <id>: ...` lines on stdout are this file's
// own contract with `scripts/hygiene/shape-roster.mjs` — no, that script
// reads `// ShapeID:` CITATIONS from `test/oracle/shape_examples.hpp`
// directly (a static scan, matching `scripts/hygiene/tier-coverage-floor.mjs`'s
// own citation convention) rather than this binary's runtime output, so the
// roster is checkable without compiling anything; this binary's `SHAPE
// PASS`/`SHAPE FAIL` lines are its own diagnostic, read only by a human or by
// `make kernel-primitive-tests`'s pass/fail sentinel below.
#include "rate_fixture_eval.hpp"

#include "src/kernel/comparator.hpp"
#include "test/oracle/metamorphic.hpp"
#include "test/oracle/rate_oracle.hpp"
#include "test/oracle/series.hpp"
#include "test/oracle/shape_examples.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace chronoduck;
using namespace chronoduck::oracle;

namespace {

int g_failures = 0;

void Check(bool condition, const std::string &what) {
	if (!condition) {
		std::fprintf(stderr, "oracle_sweep_test: FAIL — %s\n", what.c_str());
		g_failures++;
	}
}

void RunMr(const MrResult &r) {
	Check(r.pass, r.detail);
}

// -- the production adapter: OracleSample/OracleGrid -> the real ts_rate
// composition (`rate_fixture_eval.hpp`'s `EvaluateRate`), and back. This is
// the ONLY place either conversion happens.
std::vector<fixtures::RawSample> ToRaw(const std::vector<OracleSample> &samples) {
	std::vector<fixtures::RawSample> out;
	out.reserve(samples.size());
	for (const auto &s : samples) {
		out.push_back({s.t, s.v, s.has_st, s.st});
	}
	return out;
}

Grid ToGrid(const OracleGrid &g) {
	return Grid(g.start, g.end, g.step);
}

std::vector<fixtures::RatePoint> ProductionEvaluateFull(const std::vector<OracleSample> &samples,
                                                        const OracleGrid &grid, int64_t window) {
	return fixtures::EvaluateRate(ToRaw(samples), ToGrid(grid), window);
}

// Matches `metamorphic.hpp`'s generic `Evaluate` signature: this is what
// gets passed into every `Check*` relation below, so every relation in this
// file is checked against the REAL kernel composition, never against
// `rate_oracle.hpp` standing in for it.
std::vector<std::optional<double>> ProductionEvaluate(const std::vector<OracleSample> &samples, const OracleGrid &grid,
                                                      int64_t window) {
	auto full = ProductionEvaluateFull(samples, grid, window);
	std::vector<std::optional<double>> out;
	out.reserve(full.size());
	for (const auto &p : full) {
		out.push_back(p.has_value ? std::optional<double>(p.value) : std::nullopt);
	}
	return out;
}

// -- 1. ShapeID roster: one deterministic worked example per shape, checked
// three ways (expected-vs-oracle, expected-vs-production, oracle-vs-
// production), all through the one real comparator.
void TestShapeIdRoster() {
	for (const ShapeExample &ex : ShapeIdRoster()) {
		auto prod = ProductionEvaluateFull(ex.samples, ex.grid, ex.window);
		auto oracle_vals = EvaluateSeries(ex.samples, ex.grid, ex.window);
		if (prod.size() != 1 || oracle_vals.size() != 1) {
			std::fprintf(stderr, "SHAPE FAIL %s: grid did not evaluate to exactly one point\n", ex.shape_id.c_str());
			g_failures++;
			continue;
		}
		if (!prod[0].has_value || !oracle_vals[0].has_value()) {
			std::fprintf(stderr,
			             "SHAPE FAIL %s: expected a value, got null (production has_value=%d, oracle has_value=%d)\n",
			             ex.shape_id.c_str(), prod[0].has_value, oracle_vals[0].has_value());
			g_failures++;
			continue;
		}
		double scale = prod[0].scale;
		bool prod_ok = equal_values(prod[0].value, ex.expected, scale);
		bool oracle_ok = equal_values(*oracle_vals[0], ex.expected, scale);
		bool cross_ok = equal_values(prod[0].value, *oracle_vals[0], scale);
		if (prod_ok && oracle_ok && cross_ok) {
			std::printf("SHAPE PASS %s\n", ex.shape_id.c_str());
		} else {
			std::fprintf(
			    stderr,
			    "SHAPE FAIL %s: expected=%.17g production=%.17g oracle=%.17g (prod_ok=%d oracle_ok=%d cross_ok=%d)\n",
			    ex.shape_id.c_str(), ex.expected, prod[0].value, *oracle_vals[0], prod_ok, oracle_ok, cross_ok);
			g_failures++;
		}
	}
}

// -- 2. property sweep: generated series x grids x windows, oracle vs the
// real production composition (docs/testing/layers.md's L3 row).
void TestPropertySweep() {
	SplitMix64 rng(0xC0FFEEULL);
	const int kTrials = 400;
	for (int trial = 0; trial < kTrials; trial++) {
		std::size_t n = static_cast<std::size_t>(rng.UniformInt(1, 12));
		int64_t t0 = rng.UniformInt(0, 1'000'000);
		auto series = MakeMonotoneSeries(rng, n, t0, 1, 120);

		if (n >= 2 && rng.CoinFlip()) {
			std::size_t k = static_cast<std::size_t>(rng.UniformInt(1, static_cast<int64_t>(n) - 1));
			series = MakeReset(series, k);
		}
		if (!series.empty() && rng.CoinFlip()) {
			std::size_t idx = static_cast<std::size_t>(rng.UniformInt(0, static_cast<int64_t>(series.size()) - 1));
			series[idx].has_st = true;
			series[idx].st = series[idx].t - rng.UniformInt(1, 50);
		}

		int64_t step = rng.UniformInt(5, 50);
		int64_t steps = rng.UniformInt(1, 10);
		int64_t grid_start = t0 - rng.UniformInt(0, 200);
		OracleGrid grid {grid_start, grid_start + steps * step, step};
		int64_t window = rng.UniformInt(10, 500);

		auto oracle_vals = EvaluateSeries(series, grid, window);
		auto prod = ProductionEvaluateFull(series, grid, window);
		Check(oracle_vals.size() == prod.size(), "property sweep: grid size mismatch between oracle and production");
		for (std::size_t i = 0; i < oracle_vals.size() && i < prod.size(); i++) {
			bool oracle_has = oracle_vals[i].has_value();
			if (oracle_has != prod[i].has_value) {
				Check(false, "property sweep: has_value mismatch between oracle and production at grid point " +
				                 std::to_string(i));
				continue;
			}
			if (oracle_has) {
				Check(equal_values(*oracle_vals[i], prod[i].value, prod[i].scale),
				      "property sweep: oracle/production value mismatch at grid point " + std::to_string(i));
			}
		}
	}
}

// -- 3. the eval-instant sweep this issue's Goal names verbatim:
// `T in {0,15,30,60,90,120,180,300}` off the sample clock, replayed across
// several different randomly generated underlying series (not just one
// hand-derived case), each time checked oracle-vs-production.
void TestEvalInstantOffsetSweep() {
	SplitMix64 rng(0x0FF5E7ULL);
	const int kTrials = 30;
	for (int trial = 0; trial < kTrials; trial++) {
		int64_t base_t = rng.UniformInt(0, 1'000'000);
		std::size_t n = static_cast<std::size_t>(rng.UniformInt(1, 6));
		int64_t series_start = base_t - rng.UniformInt(0, 90);
		auto series = MakeMonotoneSeries(rng, n, series_start, 5, 90);
		int64_t window = rng.UniformInt(20, 400);

		OracleGrid grid = EvalInstantOffsetGrid(base_t, /*tick_scale=*/1);
		Check(grid.count() >= static_cast<int64_t>(kEvalInstantOffsetCount),
		      "eval-instant sweep: grid does not cover every named offset");

		auto oracle_vals = EvaluateSeries(series, grid, window);
		auto prod = ProductionEvaluateFull(series, grid, window);
		Check(oracle_vals.size() == prod.size(), "eval-instant sweep: grid size mismatch");
		for (std::size_t i = 0; i < oracle_vals.size() && i < prod.size(); i++) {
			bool oracle_has = oracle_vals[i].has_value();
			if (oracle_has != prod[i].has_value) {
				Check(false, "eval-instant sweep: has_value mismatch at offset index " + std::to_string(i));
				continue;
			}
			if (oracle_has) {
				Check(equal_values(*oracle_vals[i], prod[i].value, prod[i].scale),
				      "eval-instant sweep: oracle/production value mismatch at offset index " + std::to_string(i));
			}
		}
	}
}

// -- 4. the ten metamorphic relations, checked against the real production
// composition — every relation's own false-positive/false-negative note
// lives in metamorphic.hpp, next to its implementation.
void TestMetamorphicRelations() {
	SplitMix64 rng(0xABCDEF0123456789ULL);
	// `equal`'s scale for MR-SCALE: |expected| — a defensible, self-computed
	// stand-in for "the fold's own reported scale" (comparator.md: "or lets
	// the harness compute [scale] from its samples") in a context where the
	// only thing on hand is the two values being compared, not the fold's own
	// internal Sigma|terms| bookkeeping.
	auto scale_aware_equal = [](double expected, double actual) {
		return equal_values(expected, actual, std::fabs(expected));
	};

	const int kTrials = 250;
	for (int trial = 0; trial < kTrials; trial++) {
		std::size_t n = static_cast<std::size_t>(rng.UniformInt(2, 10));
		int64_t t0 = rng.UniformInt(0, 500'000);
		auto base = MakeMonotoneSeries(rng, n, t0, 5, 100);

		int64_t step = rng.UniformInt(2, 40) * 2; // even, for MR-GRID's step/2 refinement
		int64_t steps = rng.UniformInt(1, 8);
		int64_t grid_start = t0 - rng.UniformInt(0, 100);
		OracleGrid grid {grid_start, grid_start + steps * step, step};
		int64_t window = rng.UniformInt(50, 1000);

		RunMr(CheckPart(ProductionEvaluate, rng, base, grid, window));
		RunMr(CheckPerm(ProductionEvaluate, rng, base, grid, window));
		RunMr(CheckShift(ProductionEvaluate, rng, base, grid, window));
		RunMr(CheckScale(ProductionEvaluate, scale_aware_equal, rng, base, grid, window));
		RunMr(CheckReset(ProductionEvaluate, rng, base));
		RunMr(CheckSt(ProductionEvaluate, rng, base));
		RunMr(CheckDup(ProductionEvaluate, rng, base, grid, window));
		RunMr(CheckEdge(ProductionEvaluate, rng, base, grid, window));
		RunMr(CheckGrid(ProductionEvaluate, base, grid, window));
		RunMr(CheckEmpty(ProductionEvaluate, grid, window));
	}
}

} // namespace

int main() {
	TestShapeIdRoster();
	TestPropertySweep();
	TestEvalInstantOffsetSweep();
	TestMetamorphicRelations();

	if (g_failures > 0) {
		std::fprintf(stderr, "oracle_sweep_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("oracle_sweep_test: PASS\n");
	return 0;
}
