---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# Fixture values are type-checked; forbid-consumer scans fixture/function values; two Article wordings corrected

## Context

An adversarial critic pass (#168) found two related gaps, one in code behavior and one in how
`CONSTITUTION.md` describes that behavior, plus one unrelated wording overclaim found in the same
pass:

1. `scripts/fixtures-validate.mjs`'s `REQUIRED_TOP_LEVEL` loop only checked key *presence*, never
   type, for `window` and `lookback`, and never validated a `samples` entry's value beyond array
   length — unlike `grid.start/end/step`, which are type- and positivity-checked. A fixture with
   `window: "not-a-number"`, `lookback: {also: "not a number"}`, or a `HISTOGRAM`-domain sample
   whose value is a plain string instead of `test/fixtures/schema.json`'s `histogramLiteral`
   grammar all passed cleanly. `EDGE_MODES`, `DOMAINS` and `REQUIRED_TOP_LEVEL` were also hardcoded
   literals duplicating `schema.json`'s own canonical lists, with no mechanism keeping them in sync.
2. `scripts/hygiene/forbid-consumer.mjs`'s `collectKeys()` only ever collected key *names* for
   fixture files — every value, not just `provenance.*` values, was unconditionally exempt.
   `CONSTITUTION.md` Article VI.1 mischaracterized this: it said the exempt-paths list (including
   "fixture provenance values") "lives in `scripts/hygiene/consumer-tokens.json`," but that file's
   `exemptPaths` array has no such entry — the provenance-only framing didn't match the code (the
   code exempted *all* values, not just provenance ones), and the value-vs-key scoping decision
   lives in `forbid-consumer.mjs`'s logic, not the JSON config. Separately, `consumer-tokens.json`'s
   real `exemptPaths` array includes `docs/testing/corpora.md` (added legitimately by #128, which
   needs to cite consumer query-language names), which Article VI.1's prose list never mentioned —
   a second, independent drift from the same array.
3. Article II.3 read "Enforced by `make hygiene` (requires `gh`; offline runs report this check as
   red)." `forbid-ledger.mjs`'s `issueIsOpen()` — the only call site that shells out to `gh` — is
   reached only when a tracked TODO/FIXME/HACK/XXX/WIP comment citing `#<issue>` actually exists.
   With zero such comments in the tree today, an offline run never reaches that call and reports
   `PASS`, not red; the prose stated an unconditional claim for what is actually a conditional one.

## Decision

`fixtures-validate.mjs` now derives `EDGE_MODES`, `DOMAINS`, `REQUIRED_TOP_LEVEL` and the
histogram-literal field shape from `test/fixtures/schema.json` at runtime via `JSON.parse` (a
small hand-written type matcher over schema.json's own `type`/`properties` vocabulary, not a
schema-interpreter library — stays within Article IV.2's dependency-light preference), instead of
maintaining a parallel hardcoded copy. It type-checks `window` and `lookback` as numbers and
validates every `samples` entry's value against its fixture's `domain`: a plain number (or the
literal token `NaN`/`stale`, per `docs/testing/registry-and-fixtures.md`'s grammar prose) for
`COUNTER`/`GAUGE`/`NONNEG`/`ANY`, and a `histogramLiteral`-shaped object (with `schema` and
`custom_bounds` still mutually exclusive) for `HISTOGRAM`.

`forbid-consumer.mjs`'s fixture scan now additionally collects the string values of a fixture's
top-level `fixture` and `function` fields — structural identifiers (a slug, a registry function
name) with no legitimate reason to name a consumer query language — alongside key names.
`provenance.*` and every other value remain exempt, unchanged, since provenance exists to record
derivation and legitimately needs to name a source language sometimes.

`CONSTITUTION.md` Article VI.1 now lists the exempt paths exactly as `consumer-tokens.json`'s
`exemptPaths` array has them, including `docs/testing/corpora.md`, and describes the actual scan
scope precisely: a fixture's keys plus its `fixture`/`function` values, with `provenance.*` and
every other value exempt — replacing the "fixture provenance values" phrasing that implied only
provenance was exempt from an otherwise-full value scan. Article II.3 now states the actual
conditional behavior: `make hygiene` calls `gh` only to verify a deferral comment's cited issue
when such a comment exists, so offline reports red only then, and green otherwise. This governs
Article VI.1 and Article II.3. Version bumped 1.2.2 → 1.2.3, Last amended updated per Article IX.2.

## Consequences

- A fixture that smuggles a non-numeric `window`/`lookback`, or a `HISTOGRAM` sample whose value
  isn't a `histogramLiteral`, now fails `make fixtures-validate` instead of passing silently.
- `EDGE_MODES`/`DOMAINS`/`REQUIRED_TOP_LEVEL` can no longer drift from `schema.json`, since they're
  read from it rather than copied.
- A forbidden consumer-language token can no longer smuggle itself past `forbid-consumer.mjs` via a
  fixture's `fixture:` or `function:` value; it remains exempt in `provenance.*` and every other
  fixture value, unchanged.
- A future contributor reading Article VI.1 sees the exempt-paths list that actually matches
  `consumer-tokens.json`, and an accurate description of what the fixture scan actually inspects.
- A future contributor reading Article II.3 won't expect an unconditionally red offline run; only a
  tree that actually contains an unresolved deferral comment triggers the `gh` call that can go red
  offline.
