---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# State never spills; "spill" is removed from the design vocabulary entirely

## Context

Review pass 1, finding 2: the tracked allocator accounts state-heap memory but has no mechanism to
spill it to disk, which made the L11 spill test unsatisfiable as specified — a test asserting
behaviour the implementation had no way to produce. The word "spill" had been used loosely for two
different things: the operator's own buffer-managed sort (which genuinely can spill, since it's a
DuckDB buffer-manager construct) and aggregate state (which cannot).

## Decision

"Spill" is removed from every place it described aggregate state — L11 is restated as a peak-RSS
law per state class, asserted by sentinels rather than a spill test with nothing to spill. The
*only* sanctioned externalisation of state is the storable `_state`/`_merge` pattern (explicit,
caller-visible serialisation), never an implicit spill-to-disk the tracked allocator can't
actually do. The operator's own buffer-managed sort keeps its real spill behaviour, since that is
DuckDB's own external-sort mechanism, not aggregate state — see
`0003-operator-as-partition-sort-sink.md`. This governs `docs/testing/memory.md`'s L11 definition.

## Consequences

- L11's sentinels test what the tracked allocator actually does (bounded resident memory per
  state class), not a spill mechanism that was never built.
- A future state class that genuinely needs to shed memory under pressure must do it through the
  storable `_state` pattern, explicitly, not through an assumed spill path.
- The operator/aggregate distinction ("the operator's sort can spill; aggregate state never does")
  is now unambiguous in the design vocabulary instead of both being called "spill" loosely.
