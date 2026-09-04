<!-- scope: the closed roster of metamorphic relations the L3 sweep and the L9 fuzzer both check -->

# Metamorphic relations

Each relation is a named predicate the L3 sweep and the L9 fuzzer both check; the roster of relation IDs is closed and every registered function declares which relations apply (a function that declares none fails L13).

- **MR-PART** — **Partition.** For any split of the rows into partial states, in any order, `finalize(combine(parts)) ≡ finalize(state(all))` at the declared determinism class. The relation the whole kernel rests on.
- **MR-PERM** — **Permutation.** Any row order gives the same answer. Distinct from MR-PART because a single-thread run with shuffled rows exercises the sort-in-finalize path without combine.
- **MR-SHIFT** — **Time shift.** Adding a constant Δ to all timestamps and the grid leaves values unchanged, including Δ that moves the series across a day boundary and Δ that is not a multiple of step.
- **MR-SCALE** — **Value scale.** `rate(k·x) = k·rate(x)` for `k ∈ {2⁻³ … 2³}` (powers of two, so the relation is exact; other factors introduce rounding the cancelling folds cannot absorb); likewise increase, delta, deriv, sum/avg/min/max_over_time; count and changes invariant; resets invariant for `k > 0`.
- **MR-RESET** — **Reset re-basing.** Inserting a counter reset at `t` and re-basing all later samples leaves `increase` and `rate` unchanged and raises `resets` by exactly one.
- **MR-DUP** — **Duplicate rows.** Duplicating rows with identical values leaves every fold unchanged; rows with equal timestamps and differing values are resolved by the declared total order before the fold, and the relation asserts that the result is the same for every partition and row order. There is no reference for the differing-value case — the reference rejects it at ingest — so this is a ChronoDuck contract.
- **MR-EDGE** — **Window edges.** A sample at exactly `t − range` is excluded; at exactly `t` is included; moving a sample from one side of either edge to the other changes the answer only in the way the edge rule predicts.
- **MR-GRID** — **Grid refinement.** Evaluating at step `s` and at step `s/2` gives identical values at the shared grid points.
- **MR-SLICE** — **Slice additivity.** For monoid functions, `sum_over_time` over `[t−2w, t]` equals the sum of two non-overlapping half-windows; count likewise; min/max as the min/max of halves.
- **MR-MODE** — **Mode agreement.** Classic, anchored and smoothed agree exactly on a perfectly regular series whose samples fall on the window edges; they diverge only when the edge rules diverge, in the direction Rabenstein's slides predict.
- **MR-STALE** — **Staleness.** A stale marker at `t` makes resample return no value for grid points from `t` until the next non-stale sample `t′ > t`, from which the carry resumes; a gap longer than lookback yields NULL; a gap shorter yields the carried sample. The carry resumes at `t′`, not at `t + lookback` — every recovery inside the lookback window returns the carried sample, not NULL.
- **MR-HIST** — **Histogram merge.** `merge(h1,h2)` is commutative and associative; bucket counts add after downscaling to `min(scale)`; `count` and `sum` add; merging a histogram with its own zero yields itself; quantile of a merged pair lies between the quantiles of the parts.
- **MR-ST** — **Start-timestamp reset.** A value reset at `t` and a start-timestamp-only reset at `t` (values monotone, `st > prev t`) give the same `increase` and the same `resets`; with no start-timestamp role bound, the second is not a reset at all.
- **MR-EMPTY** — **Empty agreement.** When the window contains no samples, every path — oracle, kernel, SQL macro — returns the same "no value" and never a number.
