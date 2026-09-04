# Contributing

## Filing an issue

Use one of the issue forms under "New issue" (Task, Bug, or Epic) — a blank issue is disabled.
Each form asks for exactly what a reviewer needs: a Task wants its parent, goal, acceptance
criteria and out-of-scope; a Bug wants what you observed, what you expected (with a citation, not
a line number — see CONSTITUTION.md Article II.4), and acceptance criteria for the fix; an Epic
wants its outcome and what it depends on.

## Opening a pull request

The same rules apply to any pull request, whether it comes from this project's own Claude
Code-driven implementation loop or from a human contributor:

- One PR closes exactly one issue, with `Closes #N` in the body (CONSTITUTION.md Article III.1).
- The PR body has `## How`, `## Deviations`, `## Risk`, `## Evidence` (one line per acceptance
  criterion), and `## Discovered` sections, plus a `Constitution check:` line naming any articles
  touched — the pull request template pre-fills these headings.
- The PR describes execution — what you did, deviations, risk, evidence — not a restatement of
  the issue; `make pr-hygiene PR=<n>` checks this before you open the PR, not just before it
  merges.
- The PR title is [Conventional Commits](https://www.conventionalcommits.org/) format and does not
  equal the issue title verbatim.
- The diff touches only files the issue's stated scope implies (Article III.4). Discovered
  out-of-scope work becomes a new linked issue, not a scope-creeping diff.

Dependabot pull requests are exempt from the body-shape rules above (Article III.1) — they still
merge through the same green-checks loop as any other PR.

## Before you push

Run `make hygiene` and the specific `make` targets relevant to what you changed — see `AGENTS.md`
for the full command reference and `bare make` / `make help` for the live, authoritative list.

## Review

Every PR is reviewed in a fresh session against `.claude/rules/review.md` before it merges
(CONSTITUTION.md Article VIII.2); its review comment starts with `Fresh-session review:`. Two
review rounds maximum — a finding raised after that becomes a new linked issue instead of blocking
the PR further.
