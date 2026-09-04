---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# Makefile as the sole project front-end, not Justfile

This was the first ADR in this repository, written before `docs/decisions/` had the
template/numbering scaffolding T0.7 formally seeds (a MADR template file, an ADR-about-ADRs
meta-decision, and one ADR per review-log decision from the brief) — it recorded a real,
otherwise-undocumented decision that couldn't wait for that scaffolding, since Article IX.2
requires an accepted ADR in the same PR that amends the constitution. T0.7 renumbers it here
(originally `0001`) so the review-log decisions, which predate it chronologically, can occupy the
low numbers the standard MADR convention reserves for `0000`/`0001`.

## Context

T0.1 ratified Article IV.1 as "Justfile is the only project front-end... The Makefile is a shim
required by extension-ci-tools and contains only `EXT_NAME`, `EXT_CONFIG` and the include." T0.2
through T0.5 built on that: a Justfile wrapping both genuinely new tooling (the hygiene scripts,
`check-pins`, `lanes-check`, `ruleset-add-check`/`ruleset-remove-check`) and — it turned out —
targets the vendored `extension-ci-tools/makefiles/duckdb_extension.Makefile` already defines
(`release`, `test`/`test_release`, `format`/`format-check`, `tidy-check`, …), reimplemented as
thin Justfile wrappers around `make <target>`.

Having two front-ends side by side — even with a clean one-way "Justfile calls Makefile, humans
only ever type `just`" layering — was judged confusing in practice: a contributor or CI author
still has to know which of two files a given command lives in, and the Makefile can never be
fully hidden, since the distribution and code-quality reusable CI workflows
(`extension-ci-tools/.github/workflows/_extension_distribution.yml`,
`_extension_code_quality.yml`) invoke `make <target>` against this repo directly, bypassing
Justfile entirely — that path already required knowing the Makefile's target names regardless of
which front-end humans used.

## Decision

Drop Justfile. Makefile is the sole front-end. Every project-specific target (the hygiene scripts,
`check-pins`, `lanes-check`, `ruleset-add-check`/`ruleset-remove-check`, `smoke`, `test-relassert`)
is added to the root Makefile alongside the `include` of the vendored upstream Makefile, using
standard Make conventions: `.PHONY` on every target, a `## `-comment-driven `help` target as
`.DEFAULT_GOAL` (so bare `make` / `make help` lists everything, replacing `just --list`), and
`VAR=value` for recipe arguments (`make pr-hygiene PR=<n>`, `make ruleset-add-check CONTEXT=<name>`)
rather than a positional-argument hack.

A project target never shadows one the include already defines. Where the include already
provides exactly what a former Justfile recipe did (`build`→`release`, `test`, `format`,
`tidy`→`tidy-check`), the Justfile wrapper is dropped, not reimplemented — contributors use the
upstream name directly. `CMAKE_BUILD_PARALLEL_LEVEL` is `export`ed once at the top of the Makefile
rather than per-recipe, since `cmake --build` honors it natively for every cmake-driven target,
including ones the include defines that Justfile never wrapped (`debug`, `reldebug`, `wasm_*`, …).

## Consequences

- One command surface, one file, matching what CI (via the reusable workflows) already required
  in practice.
- Every already-open GitHub issue whose body names a `just` recipe needs its text corrected to the
  Make equivalent (a tracker-content fix, not a code change) so the plan stays consistent with
  what actually exists.
- Article IV.1's own wording changes from "Justfile is the only project front-end" to "Makefile is
  the only project front-end," and every other article that named `just <recipe>` as an
  enforcement mechanism (II, III, VI, VIII, IX) is updated to `make <target>` for consistency.
- `constitution-check.mjs`'s "Last amended must differ from the prior value" check is relaxed to
  "must be present" — discovered while landing this very amendment, since it and T0.1's ratification
  fall on the same calendar day and the field has no time component. The version-bump check remains
  the authoritative signal that an amendment happened; a version cannot accidentally stay the same
  the way a same-day date can.
