// rate_oracle.hpp — the from-scratch `rate` evaluator T5 requires
// (`docs/testing/rules.md`: "The from-scratch evaluator and the fixture
// harness live in a separate build target whose include path cannot reach
// the extension's sources"). Independently derived from `docs/design/`'s own
// prose spec — `docs/design/architecture.md`'s "three numeric contracts"
// paragraph and `docs/design/primitives.md`'s Tier 4 `extrapolate` row — and
// cross-checked against `test/fixtures/rate/*.yaml`'s hand-derived numbers in
// `test/kernel/oracle_sweep_test.cpp`'s `ShapeIdWorkedExamples()`; it shares
// no code, no type and no translation unit with `src/kernel/counter_fold.hpp`
// / `src/kernel/extrapolate.hpp` / `src/kernel/window_walk.hpp` — three
// separate primitives there compose into one function here on purpose, since
// nothing requires this independent evaluator to be decomposed the same way
// the kernel is.
//
// Scope, matching the one registry row this issue can exercise
// (`src/kernel/registry.def`'s `ts_rate` row): `edge_mode: EXTRAPOLATE`,
// `domain: COUNTER` only. `from_delta_temporality` is always treated as
// unknown/cumulative (the reference's own default), the same posture
// `test/kernel/rate_fixture_eval.hpp` documents for itself, since nothing in
// `test/fixtures/schema.json` carries that bit.
#pragma once

#include "series.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace chronoduck::oracle {

// -- total-order duplicate-timestamp tie-break -------------------------------
//
// `docs/design/architecture.md`'s own rule: "keep the greatest under
// totalOrder, except that any NaN loses to any non-NaN and the stale marker
// loses to an ordinary NaN." This oracle has no stale-marker concept of its
// own (STALE relations are this issue's own stated out-of-scope), so the
// three-tier rank collapses to two: NaN below every finite/infinite value,
// otherwise IEEE 754 totalOrder. Written independently as a direct
// float-compare walk rather than the kernel's bit-pattern transform — a
// different, equally valid way to implement the same documented rule.
inline bool TotalOrderLess(double a, double b) {
	bool a_nan = std::isnan(a), b_nan = std::isnan(b);
	if (a_nan != b_nan) {
		return a_nan; // NaN always loses
	}
	if (a_nan && b_nan) {
		return false; // no NaN-payload ordering needed: this oracle never generates two distinct NaNs
	}
	if (a == 0.0 && b == 0.0) {
		return std::signbit(a) && !std::signbit(b); // -0 < +0
	}
	return a < b;
}

// Sorts by timestamp; for equal timestamps, keeps the sample whose value
// wins `TotalOrderLess` (ties broken by keeping the first-seen winner, which
// never matters since two samples with the same timestamp AND the same
// total-order rank must be bit-identical values).
inline std::vector<OracleSample> SortDedup(std::vector<OracleSample> samples) {
	std::stable_sort(samples.begin(), samples.end(), [](const OracleSample &a, const OracleSample &b) {
		return a.t < b.t;
	});
	std::vector<OracleSample> out;
	out.reserve(samples.size());
	for (const OracleSample &s : samples) {
		if (!out.empty() && out.back().t == s.t) {
			if (TotalOrderLess(out.back().v, s.v)) {
				out.back() = s;
			}
			continue;
		}
		out.push_back(s);
	}
	return out;
}

// -- window selection ---------------------------------------------------------
//
// `(anchor - width, anchor]` — left-open, right-closed
// (`docs/design/architecture.md` doesn't spell this inequality itself, but
// every fixture and every registered edge mode's own boundary examples
// — `test/fixtures/rate/edge-boundary.yaml` — pin it). `data` must already be
// sorted ascending by `t` (`SortDedup`'s own postcondition).
inline std::pair<std::size_t, std::size_t> WindowRange(const std::vector<OracleSample> &data, int64_t anchor,
                                                        int64_t width) {
	std::size_t lo = 0, hi = 0;
	while (lo < data.size() && data[lo].t <= anchor - width) {
		lo++;
	}
	while (hi < data.size() && data[hi].t <= anchor) {
		hi++;
	}
	if (hi < lo) {
		hi = lo;
	}
	return {lo, hi};
}

// -- st_reset: the reference's four-case start-timestamp rule ----------------
//
// Independently transcribed from `docs/design/architecture.md`'s own prose
// ("a reset is `value drop ∨ st_reset`, where `st_reset` is the reference's
// four-case rule (unset; `ST ≥ T`; `ST < prevT`; `ST == prevT` with
// delta-versus-unknown disambiguation)"), not from
// `src/kernel/counter_fold.hpp:st_reset`'s own if-chain.
inline bool StReset(const OracleSample &prev, const OracleSample &curr, bool from_delta_temporality) {
	if (!curr.has_st) {
		return false; // unset: nothing to compare
	}
	if (curr.st >= curr.t) {
		return true; // ST >= T: not a valid "started before this sample" origin
	}
	if (curr.st < prev.t) {
		return false; // ST < prevT: consistent with the run already observed
	}
	if (curr.st == prev.t) {
		return !from_delta_temporality; // the delta-vs-unknown boundary case
	}
	return true; // strictly between prevT and T: an unambiguous reset
}

inline bool IsReset(const OracleSample &prev, const OracleSample &curr) {
	return curr.v < prev.v || StReset(prev, curr, /*from_delta_temporality=*/false);
}

// -- the fold + classic extrapolation -----------------------------------------
//
// One pass over `data[lo, hi)`, independently structured (a single loop
// rather than `counter_fold` + `extrapolate`'s own two-stage split) but
// implementing the same documented arithmetic: threshold = 1.1x the average
// inter-sample interval; a boundary gap under threshold extrapolates fully to
// the boundary, over threshold falls back to half the average interval; a
// zero clamp keeps a COUNTER's left-edge extrapolation from implying a
// negative value; a start timestamp strictly inside the left gap
// short-circuits that side to zero duration and lets a single sample yield a
// value (`docs/design/architecture.md`'s own "a single sample whose start
// timestamp lies inside the window" paragraph).
inline std::optional<double> EvaluateOnePoint(const std::vector<OracleSample> &sorted, int64_t anchor, int64_t width) {
	auto [lo, hi] = WindowRange(sorted, anchor, width);
	std::size_t n = hi - lo;
	int64_t window_start = anchor - width;

	bool left_st_inside_window = n > 0 && sorted[lo].has_st && sorted[lo].st > window_start && sorted[lo].st < sorted[lo].t;

	std::size_t eff_n = n + (left_st_inside_window ? 1u : 0u);
	if (n == 0 || eff_n < 2) {
		return std::nullopt;
	}

	int64_t eff_first_t = left_st_inside_window ? sorted[lo].st : sorted[lo].t;
	double eff_first_v = left_st_inside_window ? 0.0 : sorted[lo].v;

	double delta = 0.0;
	for (std::size_t i = lo + 1; i < hi; i++) {
		if (IsReset(sorted[i - 1], sorted[i])) {
			delta += sorted[i - 1].v;
		}
	}
	delta += sorted[hi - 1].v - sorted[lo].v;
	if (left_st_inside_window) {
		delta += sorted[lo].v; // the synthetic (st, 0.0) -> (t, v) jump folds in here
	}

	double sampled_interval = static_cast<double>(sorted[hi - 1].t - eff_first_t);
	double avg_interval = sampled_interval / static_cast<double>(eff_n - 1);
	double threshold = avg_interval * 1.1;

	double duration_to_start;
	if (left_st_inside_window) {
		duration_to_start = 0.0;
	} else {
		duration_to_start = static_cast<double>(eff_first_t - window_start);
		if (duration_to_start > threshold) {
			duration_to_start = avg_interval / 2.0;
		}
	}

	if (delta > 0.0 && eff_first_v >= 0.0) {
		double duration_to_zero = sampled_interval * (eff_first_v / delta);
		if (duration_to_zero < duration_to_start) {
			duration_to_start = duration_to_zero;
		}
	}

	double duration_to_end = static_cast<double>(anchor - sorted[hi - 1].t);
	if (duration_to_end > threshold) {
		duration_to_end = avg_interval / 2.0;
	}

	double extrapolate_to_interval = sampled_interval + duration_to_start + duration_to_end;
	double factor = extrapolate_to_interval / sampled_interval;
	double increase = delta * factor;
	return increase / static_cast<double>(width);
}

// Evaluates every point of `grid`, `window` ticks wide, from raw (possibly
// duplicate-timestamped, possibly unsorted) `samples`.
inline std::vector<std::optional<double>> EvaluateSeries(const std::vector<OracleSample> &samples,
                                                          const OracleGrid &grid, int64_t window) {
	std::vector<OracleSample> sorted = SortDedup(samples);
	std::vector<std::optional<double>> out;
	int64_t count = grid.count();
	out.reserve(static_cast<std::size_t>(count));
	for (int64_t i = 0; i < count; i++) {
		out.push_back(EvaluateOnePoint(sorted, grid.at(i), window));
	}
	return out;
}

} // namespace chronoduck::oracle
