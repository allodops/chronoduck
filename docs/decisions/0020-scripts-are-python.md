---
status: accepted
date: 2026-09-05
deciders: tsouza
---

# Scripts are Python

## Context

`scripts/` needs one scripting language, chosen against the real tooling convention of the
ecosystem chronoduck ships into. Reading the actual file trees rather than assuming their shape:

- DuckDB core's own vendored `duckdb/scripts/` carries, at any depth, 79 `.py` files and 16 `.sh`
  files and nothing else scripting anything.
- `extension-ci-tools/scripts/` — the reusable CI machinery chronoduck's own workflows `include`
  (Article IV.1) — carries 3 `.py` files (`append_extension_metadata.py`,
  `modify_distribution_matrix.py`, `configure_helper.py`) and nothing else.
- Every actively maintained third-party DuckDB extension checked from Query-farm — `crypto`,
  `lindel`, `tsid`, `fuzzycomplete` — follows the same shape and nothing wider: `extension-upload.sh`
  in all four, `scripts/bootstrap-template.py` in three of the four (`crypto`, `lindel`,
  `fuzzycomplete`; `tsid` covers the same job with `setup-custom-toolchain.sh` instead).

Independently of the DuckDB-specific evidence, DuckDB core's own `clang-tidy-diff.py` and
`run-clang-tidy.py` import `yaml` directly for their configuration files — the same shape a
`scripts/` tree parsing YAML (CI lane registries, partner pins, description manifests) needs.
PyYAML is adopted as chronoduck's YAML dependency for exactly that reason: it is what the ecosystem's
own Python tooling already reaches for, not a new choice made in isolation.

Shell coexists with Python rather than substituting for it in every one of these trees: DuckDB
core keeps 16 `.sh` files alongside its 79 `.py` files, and all four Query-farm extensions checked
ship at least one `.sh` file (`extension-upload.sh` in every one) beside their Python bootstrap
tooling — none of the four is shell-only or Python-only. chronoduck's own narrower rule follows the
same two-language shape rather than the specific scripts observed: a script whose entire job is a
short sequence of external-command invocations — no branching logic worth naming, no data
structure to hold, no library to import — doesn't need a language runtime at all, so POSIX shell is
the right fit for that job and Python is the right fit for everything else.

## Decision

`scripts/` is a Python (`.py`) tree, plus POSIX shell (`.sh`) for a script whose whole job is a
short sequence of external-command invocations. This decision governs Article IV.2 (what a script
is written in) and, downstream of it, the invocation chain Article IV.3 names
(`.github/workflows → make <target> → python3 scripts/<name>.py`).

## Consequences

- A new script is `.py` by default; `.sh` is a deliberate, narrower choice for the
  command-invocation case Article IV.2 now names, not a fallback for anything more.
- PyYAML is chronoduck's YAML library everywhere a script needs to read or write YAML (CI lane
  registries, partner pins, description manifests).
- No script in `scripts/` is written in any language other than Python, or POSIX shell under the
  narrow exception above; the hygiene scan enforcing this is itself a `scripts/` file subject to the
  same rule it enforces.
- This ADR settles the language `scripts/` is written in; it says nothing about the timing or
  scope of any individual file's language, which is left to the issues that own that work.
