---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# The range-fold operator is a partition-sort sink with law O(range) spillable + O(threads × window) resident

## Context

Review pass 1, finding 1: the per-series state as first designed was O(query range), and "bounded
memory" was false as specified — outputting one `LIST` per group required holding the whole range
resident. Review pass 2, finding F3, went further: no DuckDB extension operator inherits an
ordering contract from its input, so the *operator's* memory law had been stated as if it were the
*fold's* law (O(window)), which review pass 1 had already flagged as wrong but not fully corrected
— the grid-as-child shape and the scan-bound plan shape were unfunded, and the choice between rows
per grid point and one `LIST` per series had been decided without measurement.

## Decision

The operator is a custom sink-and-source, not an operator that trusts its input's order: it
partitions rows by series hash, sorts each partition by `ts` in buffer-managed, spillable blocks,
then streams each partition through a two-pointer walk holding one window plus edge context. Its
memory law is **O(range) in a spillable sort plus O(threads × window) resident** — never "O(window)
resident", which is the *fold's* law, not the operator's. This governs `docs/design/architecture.md`
and the L11 memory-sentinel law in `docs/testing/memory.md`.

## Consequences

- `docs/testing/memory.md`'s L11 sentinels assert the operator's actual law, not a law it never
  met.
- The reviewer's alternative (a grid index in the group key) was rejected: it replicates each
  sample `⌈window/step⌉` times, reproducing the exact row-path fan-out problem a partition-sort
  sink avoids.
- Whether the operator is expressed as a custom C++ sink or as windowed aggregates over
  grid-injected rows over DuckDB's own existing partition-sort is a separate, still-open
  measurement question — see `0011-output-shape-deferred-to-measurement.md`.
