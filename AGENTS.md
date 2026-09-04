# AGENTS.md

## Build and test

- `just build` — build the extension (`build/release/extension/chronoduck/chronoduck.duckdb_extension`).
- `just test` — run the sqllogictest suite.
- `just smoke` — LOAD the built extension into a stock DuckDB shell with `-unsigned` and assert `chronoduck_version()`.
- `just format` / `just tidy` — clang-format / clang-tidy.
- `just hygiene` — run every tree scan (forbid-ledger, forbid-consumer, verify-citations, workflow-shape, constitution-check, …); `just hygiene-selftest` proves each scan actually fails on a fixture designed to trip it.
- `just pr-hygiene <n>` — scan an open PR against Article III/VIII's rules; run before opening a PR, not just before merging.
- `just check-pins` — verify the duckdb / extension-ci-tools submodule pins agree with the workflow file.
- `just lanes-check` — verify every CI job is registered in `.github/ci-lanes.json` and vice versa.
- `just ruleset-add-check <context>` / `just ruleset-remove-check <context>` — the only way to edit required status checks on the `main` ruleset.

Until T0.2/T0.3 land, most of the above do not exist yet — an M0 issue only touches the files its own scope implies (Article III.4); do not pre-create tooling a later issue owns.

## Where things are

- `src/`, `src/include/` — the C++ kernel and DuckDB glue; `src/kernel/registry.def` is the single source of truth for registered functions (from T1.2 on).
- `test/sql/` — sqllogictest files; `test/fixtures/` — the language-neutral fixture corpus; `test/oracle/` — the from-scratch oracle (must never include `src/`).
- `docs/design/` — what the system is; `docs/testing/` — the testing discipline, binding per Article V.2; `docs/decisions/` — ADRs, the only place a "why" lives.
- `scripts/` — every script, as Bun shell modules (`.mjs`, `import { $ } from "bun"`); nothing else scripts anything.
- `.github/workflows/` — every step is `just <recipe>` after checkout/setup, or a pinned reusable `uses:` (Article IV.3).
- `.claude/rules/review.md` — the fresh-session PR review checklist (Article VIII.2).

## Working an issue

1. Claim the lowest-numbered eligible task: label `in-progress`, remove `ready`.
2. Branch `issue/<n>-<slug>` off `origin/main`.
3. Implement exactly the acceptance criteria — nothing the issue's scope doesn't imply.
4. File any discovered out-of-scope work as a new issue (with a parent and milestone) before opening the PR; link it under the PR's Discovered section.
5. Run the relevant `just` recipes before pushing.
6. Open the PR: title in Conventional Commits form, body has `Closes #n`, `## How`, `## Deviations`, `## Risk`, `## Evidence` (one line per acceptance criterion), `## Discovered`, and a `Constitution check:` line.
7. Get a review in a fresh session with only the PR number and `CONSTITUTION.md` as context, using `.claude/rules/review.md`; its comment starts with `Fresh-session review:`. Address findings on the same PR — two rounds maximum, anything after that becomes a linked issue.
8. Once required checks are green (or, pre-T0.5, once `just hygiene` passes locally and any existing CI is green), `gh-tsouza pr merge --squash --delete-branch`.
9. If the closed issue's parent task/epic/milestone now has no open children, close it too. Label the next eligible tasks `ready`.

## Gotchas

- Use `gh-tsouza`, never plain `gh`, for every GitHub CLI call in this project.
- Git pushes need the GitHub noreply identity (`122435+tsouza@users.noreply.github.com`) — GitHub's GH007 rejects a real private email as author *or* committer.
- The `main` ruleset requires `required_status_checks` to be non-empty at creation time (a GitHub API constraint the plan didn't anticipate); it starts with no required-checks rule and gets one only via `just ruleset-add-check` once a real context has reported on `main` (T0.5).
- The org's built-in issue types are `Task`, `Bug`, `Feature`; `Epic` was added manually since the plan calls for it and `Feature` isn't the right shape for a milestone root.
