// counter_fold.hpp — counter_fold and its default reset predicate
// (`value_or_st_reset`, backed by `st_reset`), a Tier 4 fold-kernel
// primitive (`docs/design/primitives.md:tier4-row-counterfold:` `first,
// last, n, reset-adjusted delta, resets, with the reset predicate as a
// plug`). Deliberately dependency-free: no `#include "duckdb.hpp"`,
// compiles standalone with a bare `g++`/`clang++ -std=c++17` — the pattern
// `window_walk.hpp` and `edge_context.hpp` established for Article V.1's
// TU-per-primitive rule.
//
// `counter_fold` itself never mentions resets, start timestamps or
// temporality: it walks `data[0..n)` once, folding a reset-adjusted delta
// and a reset count, and asks its templated `is_reset` argument — the
// primitive's own "plug" — whether each adjacent pair is a reset. This file
// also ships the one concrete plug `COUNTER`-domain rows need,
// `value_or_st_reset`, so the rule this issue's acceptance criteria pin
// (the start-timestamp four-case table) has exactly one home, but a caller
// wanting plain value-drop-only resets (the reference's own classic
// behaviour when no start timestamp is bound at all) can pass any predicate
// of matching shape instead.
#pragma once

#include <cstddef>
#include <cstdint>

namespace chronoduck {

// One counter reading: a value plus an optional start timestamp — ADR
// 0007's "ST" role
// (`docs/decisions/0007-start-timestamp-resets.md:MR-ST-sentence:` `MR-ST asserts a value reset and a`).
// `has_st` pairs with `st` the same way `EdgeContext`'s `has_before`/`before`
// pair an optional field with its own validity flag
// (`src/kernel/edge_context.hpp:EdgeContext:` `bool has_before;`).
struct CounterSample {
	int64_t t;
	double v;
	bool has_st = false;
	int64_t st = 0;
};

// st_reset — ADR 0007's four-case rule (unset; ST >= T; ST < prevT; ST ==
// prevT with delta-versus-unknown disambiguation), transcribed as an
// if-chain over the current pair `(prev, curr)`
// (`docs/design/architecture.md:three-numeric-contracts:` `with delta-versus-unknown disambiguation`):
//
//   1. unset       — `curr` carries no start timestamp at all: nothing to
//                    compare, so this predicate alone says no.
//   2. `ST ≥ T`    — `curr.st` at or after `curr.t` is not a valid "started
//                    before this sample" origin; treated as a reset.
//   3. `ST < prevT`  — `curr.st` predates the previous sample's own
//                    timestamp: consistent with the run already observed,
//                    not a reset.
//   4. `ST == prevT` — the boundary case a delta-to-cumulative conversion
//                    produces on *every* sample (each point's start is
//                    defined as the previous point's own end), so it is
//                    expected, not a reset, when the series is known to
//                    originate as delta; for anything else (cumulative-
//                    native or unknown temporality) the same equality is
//                    the tell of a fresh run beginning exactly where the
//                    last sample left off — `from_delta_temporality` is
//                    this case's own disambiguator.
//
// A value strictly between `prevT` and `curr.t` (excluded by all four named
// cases above) falls through to the same answer as case 2: the start moved
// forward since the previous sample, an unambiguous reset with no
// temporality to disambiguate.
inline bool st_reset(const CounterSample &prev, const CounterSample &curr, bool from_delta_temporality) {
	if (!curr.has_st) {
		return false;
	}
	if (curr.st >= curr.t) {
		return true;
	}
	if (curr.st < prev.t) {
		return false;
	}
	if (curr.st == prev.t) {
		return !from_delta_temporality;
	}
	return true;
}

// The concrete plug every `COUNTER`-domain row hands `counter_fold`: a
// reset is value drop ∨ st_reset
// (`docs/design/architecture.md:three-numeric-contracts:` `which matters for delta temporality`).
inline bool value_or_st_reset(const CounterSample &prev, const CounterSample &curr, bool from_delta_temporality) {
	return curr.v < prev.v || st_reset(prev, curr, from_delta_temporality);
}

// The fold's own summary — everything `extrapolate` (its own translation
// unit) needs as "a pure function of the fold summary"
// (`docs/design/primitives.md:tier4-row-extrapolate:` `as a pure function of the fold summary`),
// plus enough of the first sample's own start-timestamp state
// (`first_has_st`/`first_st`) that `extrapolate` never has to re-read
// `data[0]` itself.
struct CounterFoldSummary {
	bool has_data = false;
	double first = 0.0;
	int64_t first_t = 0;
	bool first_has_st = false;
	int64_t first_st = 0;
	double last = 0.0;
	int64_t last_t = 0;
	std::size_t n = 0;
	double delta = 0.0;
	std::size_t resets = 0;
};

// Folds `data[0..n)` — already the in-window samples, sorted ascending by
// `t` with no duplicate timestamps (`SampleBuffer::sort_dedup`'s own
// postcondition, unchecked here per `window_walk.hpp`/`edge_context.hpp`'s
// own "caller's precondition" posture) — into first/last/n/reset-adjusted
// delta/resets, reproducing the reference's own reset-adjusted increase
// walk: every reset pair adds the pre-reset value back in before the final
// `last − first` term closes the loop.
//
// `before`, when non-null, is one more sample immediately preceding
// `data[0]` (`edge_context`'s own "before" role) that `is_reset` is also
// asked about, against `data[0]` — this is `resets`' own boundary case
// (`docs/testing/primitives.md:counterfold-row:` `sample and without`): a
// caller that has a `before` sample available gets a `resets` count that
// also reflects a reset straddling the window's left edge; a caller with
// none simply passes `nullptr` and gets exactly the reference's own
// in-range-only count. `before` never contributes to `delta`, `first` or
// `n` — those describe `data[0..n)` alone.
template <typename Pred>
inline CounterFoldSummary counter_fold(const CounterSample *data, std::size_t n, const CounterSample *before,
                                       Pred &&is_reset) {
	CounterFoldSummary summary;
	if (n == 0) {
		return summary;
	}

	summary.has_data = true;
	summary.first = data[0].v;
	summary.first_t = data[0].t;
	summary.first_has_st = data[0].has_st;
	summary.first_st = data[0].st;
	summary.last = data[n - 1].v;
	summary.last_t = data[n - 1].t;
	summary.n = n;

	std::size_t resets = 0;
	if (before != nullptr && is_reset(*before, data[0])) {
		resets++;
	}

	double delta = 0.0;
	for (std::size_t i = 1; i < n; i++) {
		// The reset test itself is `is_reset`'s own strict "yes" answer, never
		// weakened to also fire on a non-answer
		// (`docs/testing/primitives.md:counterfold-row:` `on the reset test`).
		// `is_reset` is the plug; this loop does not itself compare values —
		// it only decides what a positive answer means to the fold.
		if (is_reset(data[i - 1], data[i])) {
			resets++;
			delta += data[i - 1].v;
		}
	}
	delta += data[n - 1].v - data[0].v;

	summary.delta = delta;
	summary.resets = resets;
	return summary;
}

} // namespace chronoduck
