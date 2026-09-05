// rate_fixture_eval.hpp — composes the already-registered Tier 1-4
// primitives into exactly the arithmetic `docs/design/primitives.md` says
// `rate` is: "rate, increase, resets, hist_rate and hist_increase are all
// `counter_fold` + `extrapolate` with different plugs"
// (`docs/design/primitives.md:tier4-row-header:` `with different plugs`).
// `rate` itself is not registered yet (#34, Tier 6 host glue, owns wiring it
// into `src/kernel/registry.def` and DuckDB) — this file is this issue's
// (#33, L2) own composition of the primitives that already exist, used only
// by `rate_fixture_loader.cpp`'s CLI and `rate_fixture_loader_test.cpp`'s
// direct unit test, never included from `src/`. Test-only code: it lives
// under `test/kernel/`, not `src/kernel/`, the same "helper local to its own
// test" posture `counter_fold_test.cpp`'s `ValueDropOnly` and `S(...)`
// already establish for this directory.
//
// Scope: `EDGE_MODE EXTRAPOLATE` and `domain COUNTER` only — the only
// combination `counter_fold.hpp`/`extrapolate.hpp` support today (Tier 4's
// own stated scope, `extrapolate.hpp`'s header comment: "`EDGE_MODE
// EXTRAPOLATE` only for this issue"). `from_delta_temporality` is always
// `false`: the fixture format (`test/fixtures/schema.json`) has no field for
// it, so every start-timestamp fixture this loader carries is read as
// cumulative-native or unknown temporality, never delta.
#pragma once

#include "../../src/kernel/comparator.hpp"
#include "../../src/kernel/counter_fold.hpp"
#include "../../src/kernel/extrapolate.hpp"
#include "../../src/kernel/grid.hpp"
#include "../../src/kernel/sample_buffer.hpp"
#include "../../src/kernel/window_walk.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace chronoduck::fixtures {

// One fixture-file sample: `[t, v]` or `[t, v, st]`
// (`docs/testing/registry-and-fixtures.md:fixture-format:` `(t, v) or (t, v, st)`).
struct RawSample {
	int64_t t;
	double v;
	bool has_st = false;
	int64_t st = 0;
};

// One grid point's outcome: no value at all (`has_value == false`, the
// fixture format's `null`), or a rate plus the comparator scale to check it
// against (`docs/testing/comparator.md`'s `SUM_ABS_TIMES_FACTOR`, divided by
// the nominal window width the same way the rate value itself is).
struct RatePoint {
	bool has_value = false;
	double value = 0.0;
	double scale = 0.0;
};

// Evaluates `rate` over every point of `grid`, `window` ticks wide, from the
// raw (possibly duplicate-timestamped, possibly ST-bearing) `samples`.
//
// Pipeline, each stage its own already-tested primitive:
//   1. `SampleBuffer::sort_dedup` — Tier 2 — sorts by `t` and resolves a
//      duplicate timestamp by the declared totalOrder tie-break
//      (`dedup_policy`), so the "dup" fixture family exercises the real fix,
//      not a re-implementation of it.
//   2. `window_walk` — Tier 3 — the same two-pointer walk the operator's own
//      per-partition scan uses, one `[lo, hi)` sample-index range per grid
//      point.
//   3. `counter_fold` — Tier 4 — first/last/n/reset-adjusted delta/resets
//      over each range, with `before = nullptr` (none of this issue's own
//      fixture families need a reset spanning the window's left edge).
//   4. `extrapolate` — Tier 4 — the classic boundary arithmetic, `is_counter
//      = true` throughout (every fixture this loader carries is `domain:
//      COUNTER`).
//   5. Divide by the nominal window width (ADR 0017 / `docs/design/architecture.md`'s
//      own "a single sample whose start timestamp lies inside the window
//      yields a rate" paragraph): `rate = extrapolate(...).value / window`.
inline std::vector<RatePoint> EvaluateRate(const std::vector<RawSample> &samples, const Grid &grid, int64_t window) {
	SampleBuffer buffer;
	for (const RawSample &s : samples) {
		buffer.append(s.t, s.v);
	}
	buffer.sort_dedup();

	// `st` doesn't travel through `SampleBuffer` (Tier 2's own `Sample` is
	// bare `{t, v}` — `sample_buffer.hpp`'s own struct); re-attach it after
	// dedup by timestamp. No fixture this loader carries pairs a duplicate
	// timestamp with a bound start timestamp on the same point, so a plain
	// last-write-wins map recovers the winning sample's own `st` exactly.
	std::map<int64_t, std::pair<bool, int64_t>> st_by_t;
	for (const RawSample &s : samples) {
		if (s.has_st) {
			st_by_t[s.t] = {true, s.st};
		}
	}

	SampleSpan all = buffer.slice(std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());

	std::vector<CounterSample> data;
	data.reserve(all.size());
	for (const Sample *it = all.begin; it != all.end; ++it) {
		CounterSample cs;
		cs.t = it->t;
		cs.v = it->v;
		auto found = st_by_t.find(it->t);
		if (found != st_by_t.end()) {
			cs.has_st = found->second.first;
			cs.st = found->second.second;
		}
		data.push_back(cs);
	}

	std::vector<WindowRange> ranges = window_walk(all.begin, all.size(), grid, window);

	std::vector<RatePoint> out;
	out.reserve(ranges.size());
	for (const WindowRange &r : ranges) {
		int64_t anchor = grid.at(static_cast<int64_t>(out.size()));
		int64_t window_start = anchor - window;

		const CounterSample *sub = (r.hi > r.lo) ? &data[r.lo] : nullptr;
		std::size_t n = r.hi - r.lo;
		CounterFoldSummary summary = counter_fold(sub, n, nullptr, [](const CounterSample &p, const CounterSample &c) {
			return value_or_st_reset(p, c, /*from_delta_temporality=*/false);
		});

		ExtrapolateResult ex = extrapolate(summary, window_start, anchor, /*is_counter=*/true);

		RatePoint point;
		point.has_value = ex.has_value;
		if (ex.has_value) {
			point.value = ex.value / static_cast<double>(window);
			point.scale = (std::fabs(summary.delta) * ex.factor) / static_cast<double>(window);
		}
		out.push_back(point);
	}

	return out;
}

} // namespace chronoduck::fixtures
