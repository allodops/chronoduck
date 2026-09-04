---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# Output shape (rows per grid point vs. one LIST per series) is decided by measurement, not by default

## Context

Review pass 1, finding 1 established rows-per-grid-point as the canonical operator output while
keeping the aggregate (`LIST`-per-series) form for small grids and the storable `_state`/`_merge`
pattern. Review pass 2, finding F3 found this had been decided without measurement: under a
consumer `GROUP BY`, the `LIST` form feeds a several-times-smaller intermediate — which is why
ClickHouse returns arrays — so calling either surface canonical ahead of a real benchmark asserts
a conclusion the numbers hadn't yet supported.

## Decision

Both output surfaces — rows per grid point and one `LIST` per series — are kept, and neither is
called canonical until the structural benchmark lane measures them under a representative
consumer query shape. An `operator ≡ aggregate form` parity leg is required in the test plan for
every range-fold registry row, so the two surfaces are provably equivalent in *content* regardless
of which one measurement eventually favours for *shape*. This governs `docs/design/architecture.md`'s
output-shape section and the L8 parity requirement in `docs/testing/layers.md`.

## Consequences

- No registry row or fixture family may assume one output shape is "the" shape — both are
  first-class until the benchmark decides, and the parity leg makes an accidental divergence
  between them a test failure, not a silent inconsistency.
- The structural benchmark lane has a concrete, stated job: measure both surfaces under a
  consumer `GROUP BY` shape and report which is smaller in practice, closing this decision with
  data rather than argument.
- Whether the operator itself is a custom C++ sink or windowed aggregates over grid-injected rows
  (the sink-vs-aggregate question `0003-operator-as-partition-sort-sink.md` also leaves open) is a
  related but separate question from output shape — a spike answers the former, the benchmark lane
  the latter.
