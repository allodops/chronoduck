---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# Record architecture decisions as ADRs under docs/decisions/

## Context

CONSTITUTION.md Article IX.1 places ADRs in the precedence chain between `docs/design/` and
`AGENTS.md`, and Article IX.2 requires a PR that changes the constitution to add an accepted ADR
in the same PR. Two review passes over the original brief (see the individual ADRs this one
precedes) changed real design decisions — a fence, a state law, a tie-break rule, a comparator
shape — each with reasoning worth keeping attached to the decision, not flattened into the design
docs as if it had always been obvious. Without a governed place for that reasoning, a later
disagreement has nothing concrete to argue with.

## Decision

Every design decision non-obvious enough to need a reason gets an ADR: a small MADR-shaped file
(front matter `status`, `date`, `deciders`; sections Context, Decision, Consequences) under
`docs/decisions/`, numbered `\d{4}-[a-z0-9-]+.md` in one contiguous sequence starting at `0000`
(the template itself). `status` is one of `proposed`, `accepted`, `deprecated`, `superseded`; a
`superseded` ADR names its successor. `make adr-lint` enforces the filename pattern, the
contiguous numbering, the status enum, and the ISO date on every file except the template. An ADR
records a decision, not a plan — it never describes work not yet done.

## Consequences

- A decision worth arguing with is written down once, in one place, instead of re-derived from
  design-doc prose that only states the outcome.
- `docs/decisions/0000-template.md` is the copy-paste starting point for a new ADR; `make adr-lint`
  is the enforcement, not a style guide someone has to remember to follow.
- Article IX.2's "add an accepted ADR in the same PR" requirement now has a concrete home and a
  concrete check, closing the gap `0012-makefile-as-sole-front-end.md` (formerly `0001`) had to
  work around by existing before this scaffolding did.
