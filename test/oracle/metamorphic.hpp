// metamorphic.hpp — the closed roster of metamorphic relations
// `docs/testing/metamorphic.md` names, restricted to the ten this issue
// scopes in (PART, PERM, SHIFT, SCALE, RESET, ST, DUP, EDGE, GRID, EMPTY —
// SLICE, STALE, HIST and MODE are this issue's own stated out-of-scope,
// since no registered function has a SLICE/HIST_WINDOW state, a stale-marker
// role or a second edge mode to compare yet).
//
// Every `Check*` function is templated on `Evaluate` — any callable of
// signature `std::vector<std::optional<double>>(const
// std::vector<OracleSample>&, const OracleGrid&, int64_t window)` — so this
// file names no concrete evaluator at all: `test/kernel/oracle_sweep_test.cpp`
// passes it the *real* `ts_rate` composition (`rate_fixture_eval.hpp`'s
// `EvaluateRate`, adapted), proving the relation against the thing under
// test, while a caller wanting to sanity-check the oracle against itself
// could pass `EvaluateSeries` (`rate_oracle.hpp`) instead — the same
// callable-injection shape `docs/testing/metamorphic.md`'s own scope note
// asks for ("the roster of relation IDs the L3 sweep and the L9 fuzzer both
// check"): one library, two callers, no shared state.
//
// A relation's own numeric comparison is exact (`==`, or both-absent) for
// every relation whose transform cannot perturb the floating-point
// computation at all (translation, permutation, partition, an exact no-op
// insertion, a rebased reset, an inserted exact duplicate) — proven exact in
// each function's own header comment below, not asserted by convention. Only
// MR-SCALE takes a `Compare` callable (`bool(double, double)`), because
// multiplying by a power of two is exact in isolation but the *sum* of many
// already-scaled terms is only bit-identical to the scaled *sum* under the
// same rounding decisions the comparator's own reorder budget exists to
// bound — `test/kernel/oracle_sweep_test.cpp` closes over the real
// `chronoduck::equal_values` for that one relation, so this file still
// defines no tolerance of its own (`scripts/hygiene/forbid-test-tolerance.mjs`
// has nothing to find here) and Article V.3's "one comparator" stays true:
// the only place a floating-point tolerance is EVALUATED is the caller's
// injected `Compare`, never a literal here.
#pragma once

#include "series.hpp"

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace chronoduck::oracle {

struct MrResult {
	bool pass = true;
	std::string detail; // empty when pass; a human-readable reason otherwise
};

inline MrResult MrPass() {
	return {true, ""};
}
inline MrResult MrFail(const std::string &why) {
	return {false, why};
}

namespace detail {

inline std::string PointToString(const std::optional<double> &p) {
	if (!p.has_value()) {
		return "null";
	}
	std::ostringstream oss;
	oss.precision(17);
	oss << *p;
	return oss.str();
}

// Exact-or-both-absent comparison, point by point, for the relations whose
// transform cannot perturb the floating-point computation at all.
// A single grid point whose window spans the ENTIRE series `[base.front().t,
// base.back().t]` inclusive — the shape every `test/fixtures/rate/*.yaml`
// reset/start-ts fixture already uses (a single point, `window` wide enough
// that `duration_to_start`/`duration_to_end` see both ends). MR-RESET and
// MR-ST's own invariance proofs (see their header comments) hold only when
// the evaluated window's `data[0]`/`data[last]` are the series' own true
// first/last samples on BOTH sides of the comparison — an externally-passed,
// multi-point grid could easily hand one side a strict sub-window (say, only
// the post-reset tail) whose own `data[0]` differs directly between variants,
// which would fail for a reason that has nothing to do with a bug. Building
// the window here, from the series itself, is what keeps that proof honest.
inline std::pair<OracleGrid, int64_t> FullSpanGridFor(const std::vector<OracleSample> &series) {
	int64_t first_t = series.front().t;
	int64_t last_t = series.back().t;
	int64_t window = (last_t - first_t) + 1; // strictly greater than last_t - first_t: includes first_t
	return {OracleGrid {last_t, last_t, 1}, window};
}

inline MrResult ExactEqual(const std::vector<std::optional<double>> &a, const std::vector<std::optional<double>> &b,
                           const char *relation) {
	if (a.size() != b.size()) {
		std::ostringstream oss;
		oss << relation << ": grid size mismatch (" << a.size() << " vs " << b.size() << ")";
		return MrFail(oss.str());
	}
	for (std::size_t i = 0; i < a.size(); i++) {
		bool same = a[i].has_value() == b[i].has_value() && (!a[i].has_value() || *a[i] == *b[i]);
		if (!same) {
			std::ostringstream oss;
			oss << relation << ": grid point " << i << " differs: " << PointToString(a[i]) << " vs " << PointToString(b[i]);
			return MrFail(oss.str());
		}
	}
	return MrPass();
}

} // namespace detail

// ============================================================================
// MR-PART — Partition.
// ============================================================================
// "For any split of the rows into partial states, in any order,
// finalize(combine(parts)) == finalize(state(all))" (docs/testing/metamorphic.md).
// For the `RAW_WINDOW` state class this issue's only registered row
// (`ts_rate`) declares, `combine` at the composition this L3 driver exercises
// IS concatenation: `docs/design/architecture.md`'s own "RAW_WINDOW holds
// (t, v[, st]) samples and folds them in timestamp order" already sorts the
// *whole* buffered set once in `finalize`, with no per-partial pre-reduction
// — so partitioning the rows into K groups (each independently shuffled) and
// concatenating the groups back in a shuffled order is exactly one more
// input permutation from `finalize`'s point of view. That collapse is a true
// statement about this state class, not a shortcut: L4's own row
// (`docs/testing/layers.md`) is where a *real* cross-thread combine over
// independently-finalized partial accumulators gets exercised, once a state
// class with an actual partial (`SLICE`, `HIST_MERGE`) is registered.
//
// False negative: because this composition has no genuine partial-state
// `merge()` yet, this relation cannot catch a combine bug that only exists
// in an actual merge step (an order-dependent accumulator, a partial that
// forgets to carry its own edge context) — that bug class doesn't exist at
// this composition and is L4's to catch once it does.
// False positive: none known — since `finalize` re-sorts unconditionally,
// no partition/order of the input rows should ever change the answer for a
// `D0` `RAW_WINDOW` row; a mismatch here is always a real finding.
template <typename Evaluate>
MrResult CheckPart(Evaluate &&eval, SplitMix64 &rng, const std::vector<OracleSample> &base, const OracleGrid &grid,
                   int64_t window) {
	std::size_t parts = static_cast<std::size_t>(rng.UniformInt(2, 4));
	auto scrambled = Partitioned(base, parts, rng);
	auto a = eval(base, grid, window);
	auto b = eval(scrambled, grid, window);
	return detail::ExactEqual(a, b, "MR-PART");
}

// ============================================================================
// MR-PERM — Permutation.
// ============================================================================
// "Any row order gives the same answer." Distinct from MR-PART in what it
// exercises even though both reduce to the same invariant at this
// composition (see MR-PART's own header comment): a single flat Fisher-Yates
// shuffle, with no grouping structure at all, is the minimal case MR-PART's
// grouped scramble is a superset of.
//
// False negative: a bug that depends on partition *boundaries* specifically
// (as opposed to row order in general) could in principle hide from a flat
// shuffle while MR-PART's grouped scramble still catches it — the two
// relations are kept distinct in this roster for exactly that reason, even
// though neither currently has a combine step of its own to expose such a
// bug in.
// False positive: none known, same reasoning as MR-PART.
template <typename Evaluate>
MrResult CheckPerm(Evaluate &&eval, SplitMix64 &rng, const std::vector<OracleSample> &base, const OracleGrid &grid,
                   int64_t window) {
	auto shuffled = Shuffled(base, rng);
	auto a = eval(base, grid, window);
	auto b = eval(shuffled, grid, window);
	return detail::ExactEqual(a, b, "MR-PERM");
}

// ============================================================================
// MR-SHIFT — Time shift.
// ============================================================================
// "Adding a constant delta to all timestamps and the grid leaves values
// unchanged, including delta that moves the series across a day boundary and
// delta that is not a multiple of step." Every quantity the fold's own
// arithmetic actually uses is a *difference* of two ticks (`t - window_start`,
// `last_t - first_t`, `anchor - last_t`, ...) — adding the same delta to
// every timestamp AND to the grid's own start/end leaves every one of those
// differences bit-identical (`(t+d) - (start+d) == t - start` exactly in
// two's-complement/IEEE arithmetic, no rounding introduced by the shift
// itself, so long as no term overflows `int64_t` — this relation's own
// generator keeps delta and every tick comfortably inside `int64_t`'s range).
// `window`/`step` are NOT shifted (they are durations, not anchors).
//
// False negative: a bug that corrupts the ABSOLUTE anchor (e.g. an
// off-by-a-fixed-constant error applied identically to every timestamp and
// the grid) would shift both the base and the shifted run identically and
// this relation would never notice — it only proves translation invariance,
// never absolute correctness (that is L2/L3's ShapeID worked example's job).
// False positive: a shift large enough to change which day/DST bucket a
// caller's own SQL-level TIMESTAMP arithmetic falls into could matter to a
// consumer, but this oracle's ticks are abstract `int64_t` with no calendar
// semantics at all, so no such false positive can arise here — recorded as a
// gap this relation cannot see, not a risk it silently accepts.
template <typename Evaluate>
MrResult CheckShift(Evaluate &&eval, SplitMix64 &rng, const std::vector<OracleSample> &base, const OracleGrid &grid,
                    int64_t window) {
	// One shift not a multiple of the grid step, and one large enough to model
	// "moves the series across a day boundary" (86,400 ticks, treating one
	// tick as one second the way `test/fixtures/rate/*.yaml` already does).
	int64_t not_a_multiple = grid.step > 1 ? grid.step / 2 + 1 : rng.UniformInt(1, 997);
	int64_t day = 86400;
	int64_t delta = rng.CoinFlip() ? not_a_multiple : day + rng.UniformInt(-500, 500);

	std::vector<OracleSample> shifted = base;
	for (auto &s : shifted) {
		s.t += delta;
		if (s.has_st) {
			s.st += delta;
		}
	}
	OracleGrid shifted_grid {grid.start + delta, grid.end + delta, grid.step};

	auto a = eval(base, grid, window);
	auto b = eval(shifted, shifted_grid, window);
	return detail::ExactEqual(a, b, "MR-SHIFT");
}

// ============================================================================
// MR-SCALE — Value scale.
// ============================================================================
// "rate(k*x) = k*rate(x) for k in {2^-3 .. 2^3} (powers of two, so the
// relation is exact...)". Scaling every value by a positive k preserves
// every reset's sign (v_i < v_j iff k*v_i < k*v_j for k > 0), so the reset
// boolean sequence is unchanged; the reset-adjusted delta is linear in the
// raw values, so it scales by exactly k; the zero-clamp ratio
// `eff_first_v / delta` is scale-invariant (both terms scale by k); so
// `factor` (a pure function of *timing*, once the reset pattern is fixed) is
// unchanged and `increase' = k * increase` follows algebraically. Multiplying
// a finite double by an exact power of two is itself exact (an exponent
// shift, no mantissa rounding) as long as the result doesn't overflow/
// underflow, which `k in {2^-3..2^3}` and this generator's bounded value
// range guarantee — so the *comparison* only needs the injected `equal`
// (closing over the real comparator with the real fold's own `scale`) as a
// defensive margin against the one remaining non-associativity risk: the sum
// `delta` is built one term at a time, and while each individual scaled term
// is exact, summing already-scaled terms and comparing to `k *
// sum(unscaled terms)` is only *guaranteed* bit-identical when intermediate
// rounding decisions match at every step, which they do here (same relative
// magnitudes throughout) but is worth the real tolerance rather than a bare
// `==`.
//
// False negative: this relation is blind to any bug that is itself linear in
// the input value (a constant-factor error applied uniformly to every value)
// — such a bug would satisfy `rate(k*x) = k*rate(x)` for every k despite
// being wrong for every k, since scaling by k commutes with a *second*,
// bug-induced scaling by the same wrong constant.
// False positive: a k close enough to overflow the fold's dynamic range
// (values near `DBL_MAX`) could make `k * eval(x)` overflow to infinity while
// `eval(k * x)`'s own intermediate sums overflow at a different point,
// disagreeing for a reason that has nothing to do with correctness — this
// relation's own generator stays well inside that range specifically to
// avoid manufacturing that false positive (the `inf-sub` fixture family,
// docs/testing/fixtures.md, is L1/L2's job, not this relation's).
template <typename Evaluate, typename Compare>
MrResult CheckScale(Evaluate &&eval, Compare &&equal, SplitMix64 &rng, const std::vector<OracleSample> &base,
                    const OracleGrid &grid, int64_t window) {
	static const double kPowersOfTwo[] = {0.125, 0.25, 0.5, 2.0, 4.0, 8.0};
	double k = kPowersOfTwo[rng.UniformInt(0, 5)];

	std::vector<OracleSample> scaled = base;
	for (auto &s : scaled) {
		s.v *= k;
	}

	auto a = eval(base, grid, window);
	auto b = eval(scaled, grid, window);
	if (a.size() != b.size()) {
		return MrFail("MR-SCALE: grid size mismatch");
	}
	for (std::size_t i = 0; i < a.size(); i++) {
		if (a[i].has_value() != b[i].has_value()) {
			std::ostringstream oss;
			oss << "MR-SCALE: grid point " << i << " has_value mismatch (k=" << k << ")";
			return MrFail(oss.str());
		}
		if (!a[i].has_value()) {
			continue;
		}
		double expected = *a[i] * k;
		if (!equal(expected, *b[i])) {
			std::ostringstream oss;
			oss.precision(17);
			oss << "MR-SCALE: grid point " << i << " k=" << k << " expected " << expected << " got " << *b[i];
			return MrFail(oss.str());
		}
	}
	return MrPass();
}

// ============================================================================
// MR-RESET — Reset re-basing.
// ============================================================================
// "Inserting a counter reset at t and re-basing all later samples leaves
// increase and rate unchanged and raises resets by exactly one." `MakeReset`
// (series.hpp) rebases `base[k..]` by subtracting `base[k-1].v`: every real
// increment between consecutive samples is preserved, so the reset-adjusted
// telescoping sum (`prev.v` added back at the one reset, then `last - first`
// closes the loop) reduces algebraically to the SAME `base.back().v -
// base.front().v` as the unrebased series — a bit-identical invariant, not
// an approximate one, since the same finite sequence of additions/
// subtractions in the same order produces the same floating-point result.
// Evaluated over `detail::FullSpanGridFor(base)`'s own single point spanning
// the whole series (see that helper's own comment for why an
// externally-supplied, possibly multi-point grid would invalidate the proof).
//
// This relation only checks the `rate`/`increase` half — "raises resets by
// exactly one" needs a registered `resets` row (`docs/design/surface.md`),
// which does not exist yet; that half is out of this relation's reach until
// it does, a scope gap this comment states rather than hides.
//
// False negative: a `resets`-counting bug (off-by-one, double-counting a
// reset) is entirely invisible to this relation, since it only ever reads
// `rate`'s own numeric output. False positive: none known — the rebase is
// constructed so the two series are, by the fold's own definition,
// arithmetically forced to the same delta; a mismatch is always a real
// finding (most likely: an implementation that does NOT add the pre-reset
// value back in, i.e. exactly the "reset treated as decrement" bug
// `test/fixtures/rate/reset-midwindow.yaml` names).
template <typename Evaluate>
MrResult CheckReset(Evaluate &&eval, SplitMix64 &rng, const std::vector<OracleSample> &base) {
	if (base.size() < 2) {
		return MrPass(); // nothing to split
	}
	std::size_t k = static_cast<std::size_t>(rng.UniformInt(1, static_cast<int64_t>(base.size()) - 1));
	auto reset_series = MakeReset(base, k);
	auto [grid, window] = detail::FullSpanGridFor(base);
	auto a = eval(base, grid, window);
	auto b = eval(reset_series, grid, window);
	return detail::ExactEqual(a, b, "MR-RESET");
}

// ============================================================================
// MR-ST — Start-timestamp reset.
// ============================================================================
// "A value reset at t and a start-timestamp-only reset at t (values
// monotone, st > prev t) give the same increase and the same resets; with no
// start-timestamp role bound, the second is not a reset at all." Construction:
// given a monotone `base` and a split point `1 <= k <= n-2`, variant A is
// `MakeDipAndRecover(base, k)` — a genuine value drop at k that recovers to
// end on the SAME last value `base.back().v`; variant B is `base` completely
// UNTOUCHED (still fully monotone — no value ever drops anywhere) with `st =
// t` attached on sample k, forcing `st_reset`'s `ST >= T` case. Both
// variants share `base[0..k-1]` verbatim, and both variants' fold has
// exactly one reset, at the same transition k — the reset-adjusted delta
// depends only on `data[0].v` (`base[0].v`, shared), `data[k-1].v`
// (`base[k-1].v`, shared — the term added back at the one reset) and
// `data[n-1].v` (`base.back().v`, shared BY CONSTRUCTION — `MakeDipAndRecover`
// deliberately preserves it, which `MakeReset`'s constant-offset rebase does
// NOT: that rebase changes the closing value to `base[n-1] - base[k-1]`, the
// right choice when the OTHER side of the comparison is also rebased
// (MR-RESET, above) but the wrong one here, where the other side is
// `base` unmodified). The two variants' values strictly between `k` and
// `n-1` never appear in the formula at all, so they need not match.
// Evaluated over `detail::FullSpanGridFor(base)`'s own single point spanning
// the whole series, for the same reason MR-RESET is.
//
// False negative: an implementation that ignores `st` entirely (plain
// value-drop-only reset detection, the reference's own classic behaviour
// with no start-timestamp role bound at all) makes variant B's reset vanish,
// UNDER-counting its delta — a real, catchable divergence — but an
// implementation that treats `st_reset`'s `ST >= T` case as *also* requiring
// a value drop (a strictly stronger, wrong predicate) would likewise report
// "no reset" for variant B and this relation still catches it; what this
// relation does NOT catch is a bug that gets `st_reset` right but attaches
// the wrong `prev.v` term (e.g. adds `curr.v` instead of `prev.v`) in a case
// where they happen to coincide — the generator keeps `prev.v` and `curr.v`
// distinct at the reset point on every trial specifically to close that gap.
// False positive: none known for this construction.
template <typename Evaluate>
MrResult CheckSt(Evaluate &&eval, SplitMix64 &rng, const std::vector<OracleSample> &base) {
	if (base.size() < 3) {
		return MrPass(); // needs room for the dip (at k) to recover before n-1
	}
	std::size_t k = static_cast<std::size_t>(rng.UniformInt(1, static_cast<int64_t>(base.size()) - 2));

	std::vector<OracleSample> value_reset = MakeDipAndRecover(base, k);

	std::vector<OracleSample> st_reset_variant = base;
	st_reset_variant[k].has_st = true;
	st_reset_variant[k].st = st_reset_variant[k].t; // ST >= T: st_reset's own case 2

	auto [grid, window] = detail::FullSpanGridFor(base);
	auto a = eval(value_reset, grid, window);
	auto b = eval(st_reset_variant, grid, window);
	return detail::ExactEqual(a, b, "MR-ST");
}

// ============================================================================
// MR-DUP — Duplicate rows.
// ============================================================================
// "Duplicating rows with identical values leaves every fold unchanged."
// Appending an exact bit-copy of one existing (t, v) row and re-sorting must
// collapse back to the original set: `SortDedup`'s tie-break only chooses
// between DIFFERING values (`TotalOrderLess`), and two bit-identical
// candidates always compare equal under it, so the result is bit-for-bit the
// original series regardless of which physical copy the sort happens to keep.
//
// False negative: this relation only exercises the identical-value case by
// construction (docs/testing/metamorphic.md's own text: "rows with equal
// timestamps and differing values are resolved by the declared total order
// ... There is no reference for the differing-value case"), so it cannot
// catch a broken tie-break rule for DIFFERING duplicate values (the
// 50-vs-37.5 bug `test/fixtures/rate/dup-duplicate-timestamp.yaml` and L2's
// own roster already cover that half).
// False positive: none known — an exact duplicate is a genuine no-op for any
// correct implementation of this fold.
template <typename Evaluate>
MrResult CheckDup(Evaluate &&eval, SplitMix64 &rng, const std::vector<OracleSample> &base, const OracleGrid &grid,
                  int64_t window) {
	if (base.empty()) {
		return MrPass();
	}
	std::size_t idx = static_cast<std::size_t>(rng.UniformInt(0, static_cast<int64_t>(base.size()) - 1));
	std::vector<OracleSample> with_dup = base;
	with_dup.push_back(base[idx]); // bit-identical copy, appended out of sorted order on purpose

	auto a = eval(base, grid, window);
	auto b = eval(with_dup, grid, window);
	return detail::ExactEqual(a, b, "MR-DUP");
}

// ============================================================================
// MR-EDGE — Window edges.
// ============================================================================
// "A sample at exactly t - range is excluded; at exactly t is included;
// moving a sample from one side of either edge to the other changes the
// answer only in the way the edge rule predicts." This relation asserts the
// two ALWAYS-a-no-op positions, which are provable without knowing the
// fold's numeric answer at all (`docs/testing/metamorphic.md`'s own "the
// relation... independent of what the correct numeric answer actually is"):
// a decoy sample placed at exactly `anchor - window` (the open lower bound)
// or at any `t > anchor` (past the closed upper bound) must never change the
// result, whatever value it carries — including a value engineered to
// trigger a spurious reset if it were ever wrongly admitted
// (`test/fixtures/rate/edge-boundary.yaml`'s own `999` decoy technique).
//
// False negative: this relation never asserts what MUST happen on the
// INCLUDED side (a sample exactly at `t` participating) — proving inclusion
// requires knowing the numeric answer changes in the specific, predicted
// way, which is exactly what `test/fixtures/rate/edge-boundary.yaml`'s
// worked example checks instead; a bug that wrongly EXCLUDES a should-be-
// included right-edge sample produces no violation here at all.
// False positive: none known — both asserted positions are unconditional
// no-ops for any correct implementation of this window rule.
template <typename Evaluate>
MrResult CheckEdge(Evaluate &&eval, SplitMix64 &rng, const std::vector<OracleSample> &base, const OracleGrid &grid,
                   int64_t window) {
	std::size_t target_index = static_cast<std::size_t>(grid.count() / 2);
	int64_t anchor = grid.at(static_cast<int64_t>(target_index));
	int64_t window_start = anchor - window;
	// A decoy value calibrated to be a visible outlier against the
	// surrounding baseline (kBaselineMin/kStepMax), so an implementation that
	// wrongly admits it would show up as a spurious reset or a large delta
	// shift, not silently cancel out.
	double decoy_value = kBaselineMax * 10.0;

	std::vector<OracleSample> at_left_edge = base;
	at_left_edge.push_back({window_start, decoy_value, false, 0}); // exactly the excluded lower bound
	std::vector<OracleSample> past_right_edge = base;
	past_right_edge.push_back({anchor + rng.UniformInt(1, 1000), decoy_value, false, 0}); // strictly beyond anchor

	// Compared only at `target_index`: a brand-new decoy timestamp (unlike
	// every other relation in this file, which only reorders/rebases/scales
	// EXISTING timestamps) can legitimately fall inside a DIFFERENT grid
	// point's own window and change ITS answer — a full-vector comparison
	// would misreport that legitimate difference as a violation. Only the one
	// point this decoy is positioned relative to is a provable no-op.
	auto a = eval(base, grid, window);
	auto b = eval(at_left_edge, grid, window);
	auto c = eval(past_right_edge, grid, window);
	if (target_index >= a.size() || target_index >= b.size() || target_index >= c.size()) {
		return MrFail("MR-EDGE: target index out of range");
	}
	const auto &base_point = a[target_index];
	auto same = [](const std::optional<double> &x, const std::optional<double> &y) {
		return x.has_value() == y.has_value() && (!x.has_value() || *x == *y);
	};
	if (!same(base_point, b[target_index])) {
		std::ostringstream oss;
		oss << "MR-EDGE: sample exactly at anchor - window (excluded) changed the result: "
		    << detail::PointToString(base_point) << " vs " << detail::PointToString(b[target_index]);
		return MrFail(oss.str());
	}
	if (!same(base_point, c[target_index])) {
		std::ostringstream oss;
		oss << "MR-EDGE: sample strictly past anchor (excluded) changed the result: " << detail::PointToString(base_point)
		    << " vs " << detail::PointToString(c[target_index]);
		return MrFail(oss.str());
	}
	return MrPass();
}

// ============================================================================
// MR-GRID — Grid refinement.
// ============================================================================
// "Evaluating at step s and at step s/2 gives identical values at the shared
// grid points." Each grid point's answer is a pure function of `(samples,
// anchor, window)` alone (`docs/testing/thesis.md`'s own framing) — it does
// not, and must not, depend on which OTHER points are being evaluated
// alongside it in the same call. Halving the step and re-evaluating must
// therefore reproduce, bit-for-bit, every value the coarse grid already
// reported, at the coarse grid's own anchors.
//
// False negative: a bug that corrupts every point identically regardless of
// grid resolution (a wrong constant baked into the fold itself) is invisible
// to this relation, since both grids would be wrong the same way. False
// positive: none known for a correct two-pointer or per-point evaluator;
// this relation exists specifically to catch a *performance* optimisation
// that couples neighbouring grid points incorrectly (a persisted pointer
// that assumes a fixed step between calls, say) — which is exactly the class
// of bug a naive per-point evaluator (this oracle's own `EvaluateOnePoint`)
// cannot introduce, but the real kernel's grid-wide two-pointer walk
// (`src/kernel/window_walk.hpp`) could.
template <typename Evaluate>
MrResult CheckGrid(Evaluate &&eval, const std::vector<OracleSample> &base, const OracleGrid &grid, int64_t window) {
	if (grid.step % 2 != 0) {
		return MrPass(); // step/2 wouldn't land on integer ticks; nothing to refine
	}
	OracleGrid fine {grid.start, grid.end, grid.step / 2};
	auto coarse = eval(base, grid, window);
	auto refined = eval(base, fine, window);
	for (int64_t i = 0; i < grid.count(); i++) {
		int64_t refined_index = i * 2; // grid.at(i) == fine.at(i*2) by construction
		if (static_cast<std::size_t>(refined_index) >= refined.size()) {
			return MrFail("MR-GRID: refined grid shorter than expected");
		}
		bool same = coarse[static_cast<std::size_t>(i)].has_value() == refined[static_cast<std::size_t>(refined_index)].has_value() &&
		            (!coarse[static_cast<std::size_t>(i)].has_value() ||
		             *coarse[static_cast<std::size_t>(i)] == *refined[static_cast<std::size_t>(refined_index)]);
		if (!same) {
			std::ostringstream oss;
			oss << "MR-GRID: coarse point " << i << " (anchor " << grid.at(i) << ") differs from the refined grid's own value there: "
			    << detail::PointToString(coarse[static_cast<std::size_t>(i)]) << " vs "
			    << detail::PointToString(refined[static_cast<std::size_t>(refined_index)]);
			return MrFail(oss.str());
		}
	}
	return MrPass();
}

// ============================================================================
// MR-EMPTY — Empty agreement.
// ============================================================================
// "When the window contains no samples, every path... returns the same 'no
// value' and never a number." Constructs a grid point whose window
// provably contains zero samples (every sample placed strictly before the
// window's lower edge) and asserts the evaluator reports no value there.
//
// False negative: this relation only ever tests the "genuinely no samples"
// case; it cannot catch a bug where a NON-empty window is wrongly reported
// as empty (that would need a companion assertion that a window WITH samples
// yields a value, which the ShapeID worked example and the other relations'
// own base-case evaluations already provide). False positive: none known —
// "zero samples in window" is unconditionally "no value" under every edge
// mode this kernel documents (docs/design/architecture.md's NULL_FOR_TOO_FEW
// posture), so any implementation returning a number here is simply wrong.
template <typename Evaluate>
MrResult CheckEmpty(Evaluate &&eval, const OracleGrid &grid, int64_t window) {
	int64_t anchor = grid.at(0);
	std::vector<OracleSample> far_away = {{anchor - window - 10 * (window + 1), 42.0, false, 0}};
	auto result = eval(far_away, grid, window);
	if (result.empty()) {
		return MrFail("MR-EMPTY: evaluator returned an empty grid");
	}
	if (result[0].has_value()) {
		std::ostringstream oss;
		oss << "MR-EMPTY: empty window reported a value (" << *result[0] << ") instead of null";
		return MrFail(oss.str());
	}
	return MrPass();
}

} // namespace chronoduck::oracle
