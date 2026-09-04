---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# extended_fold divides by the nominal window width, not the real elapsed span between its two edge samples

## Context

Issue #25's VictoriaMetrics source read (`app/vmselect/promql/rollup.go`) confirmed `rollupDerivFast`'s
anchor-selection rule matches `edge_context` exactly, but found a second, independent property that
`docs/design/primitives.md`'s `extended_fold` description left ambiguous: VictoriaMetrics's `rate()`
divides the anchor-to-last-sample delta by the *actual elapsed time between the two real samples
chosen* — never by the nominal window boundaries `tStart`/`tEnd` — so its denominator moves with
whatever the real sample positions happen to be. `docs/decisions/0008-extended-fold-distinct-primitive.md`
already commits `extended_fold` to the opposite choice ("divide by the window rather than the observed
interval," stated there as one of the properties that makes `extended_fold` a distinct primitive, not a
variant of `EXTRAPOLATE`), but that ADR predates checking the choice against a reference that does the
arithmetic the other way, so the tension needed resolving explicitly rather than left as an
unacknowledged gap the way `primitives.md`'s wording currently reads.

## Decision

`extended_fold` (`ANCHOR`/`SMOOTH`) keeps dividing by the nominal window width, reaffirming ADR 0008
rather than adopting VictoriaMetrics's real-elapsed-span convention. The reason: chronoduck's output is
one row per point on a regular `(start, end, step)` grid, and `ANCHOR`/`SMOOTH` exist specifically as the
no-threshold, no-clamp alternative to `EXTRAPOLATE` — real-elapsed-time division is the behavior
`EXTRAPOLATE`'s own arithmetic already provides (its factor is built from `sampledInterval` plus
boundary corrections). Dividing `extended_fold`'s output by the real anchor-to-last-sample span instead
of the nominal width would make its result move with sampling jitter alone — two adjacent grid points
over the same series could report different effective windows purely from where samples happened to
fall, undermining the one property that makes a grid useful: comparability across grid points and across
series with different sampling densities. `docs/design/primitives.md`'s `extended_fold` entry is worded
to say so explicitly and cites this ADR.

This is consequently a declared, deliberate divergence from VictoriaMetrics's own `rate()`/`increase()`
arithmetic under irregular sampling, distinct from the anchor-*selection* rule (which does match, per
`docs/design/coverage.md` row 55's "Appears in" citation of MetricsQL rate/increase/delta — that citation
covers which sample is chosen as the anchor, not what the fold divides by). VictoriaMetrics is not one of
`docs/testing/live-oracles.md`'s three live oracles (chDB, Timescale, `chplan`), so this divergence needs
no `Article V.3` comparator-enum entry today; it would need one if a MetricsQL-family live oracle is ever
added.

## Consequences

- `docs/design/primitives.md`'s `extended_fold` entry states "divided by the nominal window width, not
  the real elapsed time between `left` and `right`," citing this ADR, instead of the ambiguous "divided
  by the window" ADR 0008 left it with.
- A future fixture exercising `ANCHOR`/`SMOOTH` under irregular sampling asserts against the nominal-width
  denominator; it is expected to disagree with a hand-computed VictoriaMetrics-style real-elapsed-span
  result on the same input, and that disagreement is correct, not a bug.
- If chronoduck ever adds a live MetricsQL-family oracle, this divergence needs its own
  `ChdbDivergence`/similar enum entry (Article V.3) before any fixture can rely on it; until then it is
  simply documented kernel behavior, not an oracle-comparator concern.
