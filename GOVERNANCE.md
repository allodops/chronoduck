# Governance

## Owner

The project has one owner (see `.github/CODEOWNERS`), who holds final decision authority over
every change.

## How work happens

Every change lands through a pull request that closes exactly one issue (CONSTITUTION.md Article
III.1). Implementation, review and merging are all done by Claude Code running on the owner's
machine — one issue at a time, claimed, branched, implemented, reviewed in a fresh session against
`.claude/rules/review.md`, and merged once required checks are green (Article VII.1, Article
VIII). No agent runs inside CI; CI is checks only — build, lint, and the hygiene scans that enforce
this repository's own rules against itself.

## Decisions

CONSTITUTION.md is the ratified governing document; it supersedes every other practice. It is
amended only by a pull request that also adds an accepted ADR under `docs/decisions/` explaining
the change, bumps the document's version, and updates its "Last amended" date (Article IX.2). A
design decision worth arguing with later gets its own ADR (see
`docs/decisions/0001-record-architecture-decisions.md`) rather than being buried in a commit
message or a review comment.

## Precedence

CONSTITUTION.md → `docs/testing/` → `docs/design/` → ADRs → `AGENTS.md` → the issue (Article
IX.1). A lower-precedence document never contradicts a higher one; where they seem to, the higher
one wins and the lower one is wrong until fixed.

## Contact

See `CONTRIBUTING.md` for how to file an issue or open a pull request, `SECURITY.md` for
vulnerability reports, and `SUPPORT.md` for usage questions.
