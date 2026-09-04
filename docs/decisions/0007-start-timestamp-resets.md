---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# Counters carry an optional start timestamp, with a two-part reset predicate

## Context

Review pass 2, finding F1 (accepted, verified): the reference system's reset predicate includes
start-timestamp resets in addition to value-drop resets, and a single sample whose start timestamp
falls inside the query window yields a rate on its own — neither the fold nor the fixture grammar
as drafted could represent either case, so "one sample → no value" was being asserted as always
true when it is only true when no start-timestamp role is bound.

## Decision

Every `COUNTER`-domain function takes an optional third input, `start_ts`; the fixture literal
becomes `[t, v, st]`. A reset is `value drop ∨ st_reset`, where `st_reset` is a four-case rule
(unset; `ST ≥ T`; `ST < prevT`; `ST == prevT` with delta-versus-unknown disambiguation). A single
sample whose start timestamp lies inside the window yields a rate with `durationToStart = 0`,
`sampledInterval = t − ST`, the first value counted. MR-ST asserts a value reset and a
start-timestamp-only reset at the same point give the same `increase`. This governs
`docs/design/schema.md`'s profile shape and `docs/testing/registry-and-fixtures.md`'s fixture
format.

## Consequences

- "One sample → no value" is now a conditional fixture case, true only when no `start_ts` role is
  bound, not a blanket rule.
- Every `COUNTER` fixture family gains a start-ts variant to exercise the four-case reset rule
  and the single-sample-with-bound-start-inside-window rate.
- A writer with no start-timestamp concept (most OTel/Prometheus paths as commonly configured)
  is unaffected — the input is optional and the existing value-drop-only reset rule is exactly the
  `st_reset` "unset" case.
