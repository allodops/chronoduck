---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# ANCHOR and SMOOTH are a distinct primitive, extended_fold, not a variant of EXTRAPOLATE

## Context

Review pass 2, finding F2 (accepted, verified): `ANCHOR` and `SMOOTH` edge modes are actually a
different fold from `EXTRAPOLATE` — they interpolate both edges (or read the last sample before
the window for `ANCHOR`), correct resets over the *inside* sequence, apply no threshold, and
divide by the window rather than the observed interval. Treating them as an `EXTRAPOLATE` variant
meant the summary-only integration seam was tested against a property that does not actually hold
for them.

## Decision

`extended_fold` is a new Tier 4 primitive, distinct from the plain extrapolated range fold. The
summary-only integration seam is scoped to `EXTRAPOLATE` alone, since it is the only edge mode the
seam's property actually holds for. `edge_context` exposes the anchor sample at `t − w` so
`extended_fold` has what it needs without duplicating window-walk state. This governs
`docs/design/primitives.md`'s tier table and `docs/testing/primitives.md`'s seam table.

## Consequences

- `ANCHOR`/`SMOOTH` get their own seam tests instead of inheriting `EXTRAPOLATE`'s, which would
  silently pass on a property that doesn't apply to them.
- The Tier 4 primitive table gains a row (`extended_fold`) with its own contract, separate from the
  plain range-fold primitive it was previously conflated with.
- Histogram schema-reduction timing (when a mixed-schema merge downgrades resolution) is declared
  explicitly as part of this primitive's contract, closing a gap the conflated version left open.
