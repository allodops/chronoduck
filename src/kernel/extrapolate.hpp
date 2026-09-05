// extrapolate.hpp — extrapolate, a Tier 4 fold-kernel primitive, `EDGE_MODE
// EXTRAPOLATE` only for this issue
// (`docs/design/primitives.md:tier4-row-extrapolate:` `the reference's classic edge arithmetic`).
// Deliberately dependency-free: no `#include "duckdb.hpp"`, compiles
// standalone with a bare `g++`/`clang++ -std=c++17` — the pattern
// `counter_fold.hpp` and `window_walk.hpp` established for Article V.1's
// TU-per-primitive rule.
//
// A pure function of a `CounterFoldSummary` plus the window bounds and the
// `COUNTER`-domain flag — it never walks samples itself, never knows about
// resets or start-timestamp temporality, and takes no `Pred` template
// argument at all: `counter_fold` already reduced the walk to five numbers
// (`first`, `first_t`, `last`, `last_t`, `n`) plus `delta`, and this file's
// whole job is the reference's classic boundary arithmetic on top of those
// (`docs/design/primitives.md:tier4-row-extrapolate:` `as a pure function of the fold summary`):
// threshold = 1.1 × the average inter-sample interval, a boundary gap under
// threshold extrapolates all the way to the boundary, over threshold falls
// back to half the average interval, a zero clamp keeps a `COUNTER`'s
// left-edge extrapolation from implying a negative value, and — this
// issue's own single-sample case — a start timestamp strictly inside the
// left gap short-circuits that side to zero duration.
#pragma once

#include "counter_fold.hpp"

#include <cstddef>
#include <cstdint>

namespace chronoduck {

// `value` is the extrapolated, reset-adjusted delta (an `increase`-shaped
// answer; a `rate` divisor is the caller's own `/ window_seconds`, outside
// this primitive's scope). `factor` is `extrapolate_to_interval /
// sampled_interval` — the multiplier `ScaleKind::SUM_ABS_TIMES_FACTOR`'s own
// scale is defined against, "since the fold multiplies by the extrapolation
// factor after summing"
// (`docs/testing/comparator.md:scale-kind-list:` `for extrapolated counters, since the fold`),
// exposed here rather than folded silently into `value` so a caller's
// comparator scale can be `Σ|terms| × factor` without recomputing it.
struct ExtrapolateResult {
	bool has_value = false;
	double value = 0.0;
	double factor = 0.0;
};

// `window_start`/`window_end` are the query window's own boundary
// timestamps (`Window`'s `anchor - width`/`anchor`, in the caller's tick
// units); `is_counter` gates both the zero clamp and the start-timestamp
// short-circuit, since both are `COUNTER`-domain-only concepts ADR 0007
// introduces.
inline ExtrapolateResult extrapolate(const CounterFoldSummary &summary, int64_t window_start, int64_t window_end,
                                     bool is_counter) {
	ExtrapolateResult result;

	// The single-sample case this issue's own acceptance criteria name: a
	// start timestamp `summary.first_st` strictly inside the left gap
	// `(window_start, summary.first_t)` is a known true-zero point, not a
	// gap to guess across
	// (`docs/design/architecture.md:three-numeric-contracts:` `a single sample whose start timestamp lies inside the
	// window yields a rate`). Modelled as one more (synthetic) sample at `(first_st, 0.0)` ahead of `summary.first`: it
	// lengthens the effective sample count by one, replaces the effective "first" point, and its jump from 0 up to
	// `summary.first` is folded into `eff_delta` alongside whatever
	// `counter_fold` already summed among the real in-window samples —
	// after which the rest of this function's arithmetic runs unchanged.
	bool left_st_inside_window = is_counter && summary.has_data && summary.first_has_st &&
	                             summary.first_st > window_start && summary.first_st < summary.first_t;

	std::size_t eff_n = summary.n + (left_st_inside_window ? 1u : 0u);
	if (!summary.has_data || eff_n < 2) {
		return result; // "not enough samples" → no value, every mode alike
	}

	int64_t eff_first_t = left_st_inside_window ? summary.first_st : summary.first_t;
	double eff_first_v = left_st_inside_window ? 0.0 : summary.first;
	double eff_delta = left_st_inside_window ? (summary.delta + summary.first) : summary.delta;

	double sampled_interval = static_cast<double>(summary.last_t - eff_first_t);
	// `n − 1`, never `n`: `eff_n` counts *points*, and an average *interval*
	// divides the total span by the number of gaps between them, one fewer
	// than the point count
	// (`docs/testing/primitives.md:extrapolate-row:` ` in the average interval`).
	double avg_interval = sampled_interval / static_cast<double>(eff_n - 1);
	// The 1.1× threshold itself
	// (`docs/testing/primitives.md:extrapolate-row:` `any other constant`).
	double threshold = avg_interval * 1.1;

	double duration_to_start;
	if (left_st_inside_window) {
		duration_to_start = 0.0;
	} else {
		duration_to_start = static_cast<double>(eff_first_t - window_start);
		// Strict `>`, never `>=`: a gap exactly *at* the threshold still
		// extrapolates all the way to the boundary; only a gap that exceeds
		// it falls back to the half interval below
		// (`docs/testing/primitives.md:extrapolate-row:` ` at the threshold;`).
		if (duration_to_start > threshold) {
			duration_to_start = avg_interval / 2.0; // half the average interval, never the bare interval
		}
	}

	// The `COUNTER` zero clamp: only ever *shrinks* `duration_to_start`
	// (`docs/testing/primitives.md:extrapolate-row:` `clamp direction`) — the
	// comparison below is one-directional by construction, never a
	// magnitude test either side could win.
	if (is_counter && eff_delta > 0.0 && eff_first_v >= 0.0) {
		double duration_to_zero = sampled_interval * (eff_first_v / eff_delta);
		if (duration_to_zero < duration_to_start) {
			duration_to_start = duration_to_zero;
		}
	}

	double duration_to_end = static_cast<double>(window_end - summary.last_t);
	if (duration_to_end > threshold) {
		duration_to_end = avg_interval / 2.0;
	}

	double extrapolate_to_interval = sampled_interval + duration_to_start + duration_to_end;
	double factor = extrapolate_to_interval / sampled_interval;

	result.has_value = true;
	result.value = eff_delta * factor;
	result.factor = factor;
	return result;
}

} // namespace chronoduck
