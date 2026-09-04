---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# D0-class sums use reproducible summation; Neumaier compensation stays for D1

## Context

Review pass 1, finding 5: bucket-ordered Neumaier-compensated summation does not yield D0
(bit-deterministic under any partition) — compensation bounds the error a summation accumulates,
but it does not make the *result* independent of the order values are summed in, since a
compensated sum over a different bucket order still produces a different (if similarly accurate)
answer. Review pass 2, finding F5, located the actual cost this would impose: the risk from
reproducible summation is in re-binning on `combine`, not per-sample overhead, and that risk needs
a benchmark to actually decide the question rather than an assumption.

## Decision

The `D0` sum path uses reproducible summation (order-independent by construction), and the `D1`
path keeps Neumaier-compensated addition, with the compensation term carried in the partial state.
Re-binning behaviour on `combine` is specified explicitly, and MR-PART (a metamorphic relation
asserting partition-invariance) is run at wide dynamic range as the actual test of the re-binning
risk. A four-accumulator benchmark, with a stated pass criterion, decides whether `D1` can retire
in favour of `D0` everywhere once reproducible summation's overhead is measured rather than
assumed. This governs `docs/testing/determinism.md`.

## Consequences

- `D0`-class functions are bit-deterministic under any partition scheme by construction, not by
  hoping compensated arithmetic happens to agree across runs.
- `D1` remains a real, distinct determinism class as long as the four-accumulator benchmark hasn't
  cleared reproducible summation for full replacement — the two classes coexist deliberately, not
  as an oversight.
- MR-PART at wide dynamic range is the concrete, executable form of "the re-binning risk is
  bounded," replacing what would otherwise be an unverified claim.
