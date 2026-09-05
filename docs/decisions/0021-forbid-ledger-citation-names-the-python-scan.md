---
status: accepted
date: 2026-09-05
deciders: tsouza
---

# forbid-ledger citation names the Python scan

## Context

Article II.2 names the file that enforces the ledger denylist by path, and that path does not
resolve to anything under `scripts/hygiene/`: the scan Article II.2 describes lives at
`scripts/hygiene/forbid-ledger.py`, wired into `make hygiene` by `scripts/hygiene.py`'s
`TREE_SCANS` list. Article II.4 requires a citation to name a construct that actually occurs in
the file it names; a path citation is the same discipline applied to `CONSTITUTION.md` itself, and
the article currently fails it.

Article IV.2 settled `scripts/` as a Python (`.py`) tree, with POSIX shell (`.sh`) for the narrow
external-command-invocation case (ADR 0020). Every script the constitution or its own hygiene
scans cite by path should name the file that decision produced, not a path no scan or Makefile
target resolves against.

## Decision

Article II.2's citation is corrected to `scripts/hygiene/forbid-ledger.py`, the file that
implements the ledger denylist scan today. No other part of Article II changes: the denylist set,
the `CHANGELOG.md` marker rule and the per-PR/release-time split are untouched.

## Consequences

- `CONSTITUTION.md` cites the same file its own `make hygiene` invocation resolves against,
  restoring Article II.4's citation discipline to the constitution's own text.
- A future rename of `scripts/hygiene/forbid-ledger.py` needs the same Article IX.2 amendment
  this one follows -- the citation is part of the constitution's text, not an implementation
  detail free to drift silently.
