---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# The performance comparables table is reduced to cerberus and the L6a oracles

## Context

Review pass 2, finding F10 (accepted): TSBS, MQE and Thanos rows in the performance comparables
table are not actually comparables — they measure different things under different conditions,
and treating their numbers as directly comparable to ChronoDuck's own manufactures a precision the
underlying benchmarks don't support. Related findings F8 and F9 in the same pass found the
advance-over-baseline targets had been copied without the constraints that made them meaningful in
their original context (best-of-N hiding the parallel tail, RSS as the wrong number for a state
law, the MQE figure in particular resting on assumptions that don't transfer).

## Decision

The comparables table is reduced to cerberus and the L6a differential-testing oracles — systems
ChronoDuck already runs the same fixtures against under the same comparator, where a number means
what it claims to mean. TSBS, MQE and Thanos are demoted to *shape sources* (informing what
workloads and query patterns are worth benchmarking) rather than comparables (numbers claimed to
be measuring the same thing). This governs `docs/performance.md`'s comparables section.

## Consequences

- Every number in the comparables table is now measured under a comparator ChronoDuck itself
  controls, not borrowed from an external benchmark with different conditions.
- Speedup and ratio claims are stated relative to same-day chDB numbers and the operator's own
  timing, not a cross-benchmark figure that doesn't hold up under its own constraints.
- Memory numbers are reported in tracked state bytes per window sample (the unit the L11 law
  actually asserts), not RSS, which review pass 2's F8 found to be the wrong number for a state
  law in the first place.
