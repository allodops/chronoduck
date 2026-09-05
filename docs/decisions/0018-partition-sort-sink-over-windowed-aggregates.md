---
status: accepted
date: 2026-09-05
deciders: tsouza
---

# The range-fold operator is a custom partition-sort sink, not windowed aggregates over grid-injected rows

## Context

ADR 0003 fixed the *internal shape* of the range-fold operator if a custom sink were chosen
(partition by series hash, spillable per-partition sort by `ts`, a two-pointer walk holding one
window plus edge context) but left open, per its own closing line, "whether the operator is
expressed as a custom C++ sink or as windowed aggregates over grid-injected rows over DuckDB's own
existing partition-sort" — deferred to issue #39's spike, per ADR 0011.

The spike built both as working, measurable throwaway prototypes (not just reasoned about them),
benchmarked on the exact cerberus shape `docs/performance.md` calibrates against
(`rate-sum-5m-15s`, 500k samples: 2000 series × 250 samples × 15s cadence, 300s window), same host,
same day, back-to-back, cross-checked against chDB's native `timeSeriesRateToGrid` on the same data:

- **Prototype A** (partition-sort sink, proxied by a hand-wired parallel `TableFunction`): wall
  ≈0.22s median (≈0.6× chDB's ≈0.37s — *faster*, not just within the 2× target), resident bytes
  peak ≈73–83 KB.
- **Prototype B** (custom windowed `AggregateFunction` used with a plain SQL `OVER (...)` clause
  over grid-injected rows, no custom operator code): wall ≈1.86s median (≈5× chDB — misses the ≤2×
  target), resident bytes peak ≈129 MB.
- Both were re-run and independently confirmed during this decision's own review (A: 0.26s /
  72,864 B; B: 2.48s / 129,546,112 B) — consistent with the spike's reported ranges.
- The two prototypes' resident-byte gap, measured with identical instrumentation
  (`SampleBuffer::byte_count()`) on identical data, is **≈1700–1800×**, independent of any weak
  cross-process chDB memory proxy. This is DuckDB's generic segment-tree/window-evaluation
  machinery applied to a custom aggregate under `OVER (...)`, not a tuning artifact either
  prototype's author introduced.
- Both prototypes were fixture-equal (12/12 fixtures under `test/fixtures/rate/` and
  `test/fixtures/derived/rate/`, checked against `src/kernel/comparator.hpp`'s real tolerance) —
  but a full 500,000-row cross-check between the two prototypes' *live SQL* outputs (beyond the
  spike's formal acceptance criterion) caught a real bug the fixture corpus missed: prototype B's
  naive SQL `RANGE` frame was closed on both edges while `window.hpp`'s `Window::contains`
  convention is half-open, disagreeing on 445,428 of 498,000 non-null rows until fixed. This is
  itself evidence for a design property, not just a bug report: prototype A's shape composes
  directly with the kernel's own half-open `Window` primitive, while prototype B's shape requires a
  second, independent encoding of the same boundary rule in SQL frame syntax, which drifted.
- Fold-forms feasibility (`docs/design/surface.md`'s bounded/anchored/multi):
  - **Bounded** is easy for both, and simpler than either grid-walking prototype.
  - **Anchored** — the form named in issue #39 as the open, hard question — is essentially free
    for prototype B (an ordinary SQL join against an arbitrary anchor relation; demonstrated live
    with 3 irregular anchor timestamps, not just asserted) but requires genuine, unbuilt
    engineering for prototype A: a two-child sink is expressible (confirmed by reading DuckDB's
    vendored `PhysicalAsOfJoin` — two independent per-side sink states dispatched by a `child`
    index, built on generic `SortStrategy`/`MetaPipeline`, nothing join-specific in
    `PhysicalOperator`'s own virtuals — and `duckdb-spatial`'s `PhysicalSpatialJoin` as a second,
    non-join precedent), but only via an `OptimizerExtension` rewriting the logical plan into a
    custom `LogicalExtensionOperator` whose `CreatePlan` emits the two-child physical operator — an
    out-of-tree extension cannot substitute a physical operator without that logical-plan entry
    point. This is real, T2.2-scale engineering, not a spike-sized gap.
  - **Multi** (several folds from one walk into a `STRUCT`) is add-a-fold-function-easy for
    prototype A (the same `[lo, hi)` ranges serve N fold functions at zero extra walk cost) but
    requires bundling every fold member into one hand-written aggregate's `Finalize` for prototype B
    to avoid paying its per-fold memory/wall cost N times over — a real ergonomic asymmetry, not a
    hard blocker.

## Decision

The range-fold operator is implemented as a **custom C++ sink-and-source physical operator**
(prototype A's shape), per ADR 0003's already-fixed internal design — partition by series hash,
spillable per-partition sort by `ts`, a two-pointer walk holding one window plus edge context,
released at each series boundary. This governs `docs/design/architecture.md`'s "Where it plugs in"
section and closes ADR 0011's remaining open question (output *shape*, rows-per-grid-point vs.
`LIST`-per-series, is unaffected and remains genuinely open per that ADR — this decision is about
the *mechanism*, not the output surface).

Windowed aggregates over grid-injected rows (prototype B's shape) are rejected for the grid/RANGE
case: the measured wall-clock cost (≈5× chDB, missing the stated ≤2× target) and resident-memory
cost (≈1700–1800× prototype A's, on identical data and instrumentation) are real costs of DuckDB's
generic window-evaluation machinery applied to a custom aggregate, not artifacts either prototype's
author introduced by under-tuning. Both prototypes reflect a straightforward, idiomatic build of
each architecture, not either one's theoretical best case (prototype A used a hand-rolled thread
pool rather than DuckDB's native parallel-table-function scheduler and held whole per-partition
ranges resident rather than implementing ADR 0003's allowed spill; prototype B made no attempt to
steer DuckDB away from its default window-evaluation strategy) — but the gap between them is wide
enough (multiple orders of magnitude on memory, several-fold on wall) that a plausible amount of
further tuning on either side would not close it.

Issue #39's own two prototypes are throwaway (`spike/`, untracked) and are deleted as part of this
decision landing — prototype A's shape is carried forward as the real, production
implementation's starting point in issue #40 (T2.2), not its literal code (the spike's hand-rolled
thread pool and no-spill design are explicitly named above as simplifications the real
implementation must not inherit unmeasured).

The anchored fold form (`docs/design/surface.md`'s "forms" section) is expressible for this choice
via `OptimizerExtension` + `LogicalExtensionOperator`, per the precedent-reading evidence above, but
is real, unbuilt, T2.2-scale-or-larger engineering, not included in issue #40's initial scope — it
remains "a candidate, not a row" per `docs/design/architecture.md`'s existing framing, to be picked
up as its own follow-on issue once the primary (grid) sink form is production-complete.

## Consequences

- `docs/design/architecture.md`'s open question ("build the sink, or express the range folds as
  windowed aggregates ... which of the two is chosen is an open question an ADR decides") is
  closed: the sink. That document's own text should be updated to state this as settled rather than
  open, in the PR that lands this ADR.
- Issue #40 (T2.2, "The operator: grid_stream over partition-sorted input") proceeds against a
  measured, not assumed, foundation — but must independently earn ADR 0003's memory law
  (O(range) spillable + O(threads × window) resident) rather than inherit the spike's own
  whole-partition-resident, no-spill numbers, which were an explicitly named simplification, not a
  validated target.
- The bounded fold form is unaffected by this decision (both architectures handle it trivially);
  the anchored form carries a known, named, deferred engineering cost that a future issue must
  size and schedule rather than discover from scratch.
- The multi-provenance methodology point from the spike — a small hand-written fixture corpus did
  not catch a real live-SQL frame-boundary bug that a full-dataset cross-check did — is worth
  carrying into how future operator-vs-reference parity work (issue #44, L8 cross-path proofs) is
  designed: a fixture-only equality claim is not sufficient evidence for a live query-execution
  path, only for the shared fold math a fixture corpus actually exercises.
