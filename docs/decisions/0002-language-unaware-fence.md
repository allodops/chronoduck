---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# The forbid-consumer scan is scoped to code and fixture keys, not fixture values

## Context

Review pass 1, finding 8: provenance strings in fixtures violated the language-unaware token scan
as first specified, no histogram fixture literal existed to test against, and the derivation tool
that turns upstream corpora into fixtures was itself described as "a PromQL parser" — language the
scan itself would have rejected had it appeared in this repository, since a provenance string
naming where a fixture's numbers came from necessarily names the reference it came from.

## Decision

The forbidden-token scan (`scripts/hygiene/forbid-consumer.mjs`, backed by
`scripts/hygiene/consumer-tokens.json`) is scoped to code and fixture *keys* — never fixture
*values* (provenance strings, pattern text). A histogram fixture literal grammar exists so
histogram fixtures have somewhere to live without hand-rolled ad hoc encoding. The derivation tool
that reads upstream corpora is named for what it is (a consumer of the kernel, living in a
separate repository per Article VI.2) rather than pretending the kernel could stay unaware of it
existing at all. This governs `docs/testing/rules.md`'s T14 and Article VI.1.

## Consequences

- A fixture's `provenance` field can say `"prometheus 3.13, promqltest suite"` without failing the
  scan that would reject the same string appearing in kernel code.
- The scan's exempt-paths list (`CONSTITUTION.md`, `docs/prior-art.md`, `docs/decisions/`,
  `docs/design/ecosystem.md`, `docs/design/coverage.md`, plus per-file exemptions like
  `docs/testing/corpora.md`) is a separate, narrower mechanism for whole *files* that need to name
  systems in prose — fixture-value scoping handles the *data* case without needing a file on that
  list.
- The derivation tool is out-of-repo work, costed and tracked as its own project, not a kernel
  dependency.
