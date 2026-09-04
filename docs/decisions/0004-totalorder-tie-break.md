---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# Duplicate timestamps resolve by IEEE totalOrder, keep-greatest, with an explicit NaN/stale ranking

## Context

Review pass 1, finding 4: "keep max" is not a total order once NaN, the stale marker, and
histograms are in scope, and it was not the reference system's rule either — it was an invented
convenience with no source. Review pass 2, finding F6, sharpened this further: the tie-break as
drafted did not say which end of a tie is kept, did not handle `±0`, and claimed to match
ClickHouse's documented behaviour without actually doing so.

## Decision

Duplicate timestamps within a series are resolved by IEEE 754 `totalOrder` (`−0 < +0`, NaN payloads
ordered by their bit pattern), keeping the *greatest* value under that order — except that any NaN
loses to any non-NaN, and the stale marker (a specific NaN payload) loses to an ordinary NaN.
Histograms tie-break on `(count, compact-then-xxh3)` with the hash function named and pinned by a
fixture. This is a ChronoDuck contract with no reference system behind it — the brief's claim to
match ClickHouse's documented rule is dropped, since it does not. The operator deduplicates within
a series run itself and never relies on the sort being stable to do it. This governs
`docs/design/architecture.md`'s numeric-contracts section.

## Consequences

- The tie-break is total (every pair of values has a defined winner), which a bare "keep max"
  never was for NaN or the stale marker.
- A fixture family exists specifically for duplicate-timestamp cases at each value domain,
  including histograms, so the rule is tested rather than assumed.
- No comparator against an external reference can be built for this behaviour, since no reference
  defines it — the fixture family *is* the specification.
