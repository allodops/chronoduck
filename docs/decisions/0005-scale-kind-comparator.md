---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# The comparator tolerance is derived per scale_kind, not one Σ|terms| bound for every function

## Context

Review pass 1, finding 6: a relative error bound with no floor rejects correct answers from
cancelling folds and hides a dropped compensation term — the fix at the time was to parameterise
the comparator by the fold's `Σ|terms|`. Review pass 2, finding F4, found that parameterisation
itself insufficient: `Σ|terms|` is undefined or actively misleading for selections (first/last,
which have no summation error at all), quotients, recurrences, and libm-backed paths (sqrt, exp),
each of which accumulates error by a different mechanism than plain summation.

## Decision

Every registry row declares a `scale_kind` — a closed enum (at minimum: summation-derived,
`NORM2_LOGN`, `RECURRENCE(n)`, `LIBM_EXP`, and `EXACT` for selections) — and the comparator's
tolerance is derived from that kind, statically checked against the row's actual computation
shape rather than trusted as an annotation. Cancellation within a `SUM_ABS`-style fold is
explicitly attributed to the L1a fixture table, not folded into the general bound. This governs
`docs/testing/comparator.md`.

## Consequences

- A selection function (`first_over_time`) is asserted `EXACT`, not given slack a summation-derived
  bound would otherwise grant it by accident.
- Adding a new registry row means declaring its `scale_kind`, not inheriting a generic tolerance —
  a static check fails a row whose declared kind doesn't match what it actually computes.
- The comparator derivation itself becomes the one place tolerance reasoning lives, per Article
  V.3's "no tolerance file, no allow-list" — `scale_kind` is a closed enum entry, not an escape
  hatch.
