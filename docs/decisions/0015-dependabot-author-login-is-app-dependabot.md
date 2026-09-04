---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# Dependabot's PR author login is `app/dependabot`, not `dependabot[bot]`

## Context

Article III.1 named `dependabot[bot]` as the exempt author. `scripts/pr-hygiene.mjs`'s exemption
check compared `prAuthor === "dependabot[bot]"` against exactly that string — and never matched,
because `gh pr view --json author` (the API call `pr-hygiene.mjs` actually makes) reports a real
Dependabot PR's login as `app/dependabot`, confirmed against six real, closed Dependabot PRs in
this repo (#147–#152). `dependabot[bot]` is the commonly-seen *git commit author* string for
Dependabot's commits, and appears elsewhere on GitHub (e.g. `@mentions`), but it is not the login
this particular GraphQL-backed CLI query returns. #162/#163 fixed the code; this amendment brings
Article III.1's prose in line with what the code — and the real API — actually says.

## Decision

Article III.1 now names the actual field `pr-hygiene.mjs` checks (`gh pr view --json author`'s
login, `app/dependabot`) instead of the git-commit-author string `dependabot[bot]`, which never
appears in that field. `scripts/pr-label.mjs`'s own bot-detection (`.endsWith("[bot]")`, reading
a different API surface — the REST `/pulls/{number}` endpoint) is untouched; this ADR only concerns
the string Article III.1 and `pr-hygiene.mjs` share.

## Consequences

- `CONSTITUTION.md` and `scripts/pr-hygiene.mjs` now agree on the exact string that identifies a
  Dependabot PR for Article III.1's purposes.
- A future contributor reading Article III.1 won't be misled into "fixing" `pr-hygiene.mjs` back
  toward the string that doesn't work.
