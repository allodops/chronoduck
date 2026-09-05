# AGENTS.md

## Build and test

`make help` (or bare `make`) lists every recipe with a one-line description — treat it as the
live source of truth over this list. Everything is a target on the one root `Makefile`, which
`include`s the vendored `extension-ci-tools/makefiles/duckdb_extension.Makefile` — several targets
(`release`, `debug`, `reldebug`, `relassert`, `test`/`test_release`/`test_debug`, `format`/
`format-check`, `tidy-check`, `clean`, `update`, …) come from that include and aren't redefined
here; run them by their upstream name directly.

- `make release` — build the extension (`build/release/extension/chronoduck/chronoduck.duckdb_extension`); `make debug`/`make reldebug`/`make relassert` build the other configurations.
- `make test` — run the sqllogictest suite against the release build; `make test-relassert` runs it against the relassert build (no upstream `test_relassert` pairing exists, so this one's ours).
- `make smoke` — LOAD the built extension into a stock DuckDB shell with `-unsigned` and assert `chronoduck_version()`.
- `make format` / `make format-check` / `make tidy-check` — clang-format (fixes in place) / clang-format (check only, what CI runs) / clang-tidy (what CI runs).
- `make hygiene` — run every tree scan (forbid-ledger, forbid-consumer, verify-citations, workflow-shape, constitution-check, …); `make hygiene-selftest` proves each scan actually fails on a fixture designed to trip it.
- `make pr-hygiene PR=<n>` — scan an open PR against Article III/VIII's rules; run before opening a PR, not just before merging.
- `make check-pins` — verify the duckdb / extension-ci-tools submodule pins agree with the workflow file.
- `make lanes-check` — verify every CI job is registered in `.github/ci-lanes.json` and vice versa.
- `make ruleset-add-check CONTEXT=<name>` / `make ruleset-remove-check CONTEXT=<name>` — the only way to edit required status checks on the `main` ruleset.
- `make description-validate` — validate `docs/community/description.yml` with hand-rolled checks documented by (not read from) `scripts/vendor/description.schema.json`.
- `make changelog` / `make changelog-check` — write `CHANGELOG.md` from Conventional-Commit titles since the last tag, or fail if it's stale.
- `make release-checklist` — print the steps for cutting a release.

An M0 issue only touches the files its own scope implies (Article III.4); do not pre-create tooling a later issue owns.

## Where things are

- `src/`, `src/include/` — the C++ kernel and DuckDB glue; `src/kernel/registry.def` is the single source of truth for registered functions (from T1.2 on).
- `test/sql/` — sqllogictest files; `test/fixtures/` — the language-neutral fixture corpus; `test/oracle/` — the from-scratch oracle (must never include `src/`).
- `docs/design/` — what the system is; `docs/testing/` — the testing discipline, binding per Article V.2; `docs/decisions/` — ADRs, the only place a "why" lives.
- `scripts/` — every script, as Python (`.py`), except a script whose entire job is a short sequence of external-command invocations, which is POSIX shell (`.sh`) instead (Article IV.2); nothing else scripts anything.
- `.github/workflows/` — every step is `make <target>` after checkout/setup, or a pinned reusable `uses:` (Article IV.3).
- `.claude/rules/review.md` — the fresh-session PR review checklist (Article VIII.2).

## Working an issue

1. Claim the lowest-numbered eligible task: label `in-progress`, remove `ready`.
2. Branch `issue/<n>-<slug>` off `origin/main`.
3. Implement exactly the acceptance criteria — nothing the issue's scope doesn't imply.
4. File any discovered out-of-scope work as a new issue (with a parent and milestone) before opening the PR; link it under the PR's Discovered section.
5. Run the relevant `make` targets before pushing.
6. Open the PR: title in Conventional Commits form, body has `Closes #n`, `## How`, `## Deviations`, `## Risk`, `## Evidence` (one line per acceptance criterion), `## Discovered`, and a `Constitution check:` line.
7. Get a review in a fresh session with only the PR number and `CONSTITUTION.md` as context, using `.claude/rules/review.md`; its comment starts with `Fresh-session review:`. Address findings on the same PR — two rounds maximum, anything after that becomes a linked issue.
8. Once required checks are green and `make pr-hygiene PR=<n>` passes — which enforces that a
   `Fresh-session review:` comment exists and postdates the PR's last commit (Article VIII.2) —
   `gh-tsouza pr merge --squash --delete-branch`.
9. If the closed issue's parent task/epic/milestone now has no open children, close it too. Label the next eligible tasks `ready`.

## Gotchas

- Use `gh-tsouza`, never plain `gh`, for every GitHub CLI call in this project.
- `CHRONODUCK_GH_INTERACTIVE_CLI` (default `gh`) lets an operator point the 5 scripts that shell
  out to the GitHub CLI interactively — `scripts/coverage-check.py`, `scripts/ruleset.py`,
  `scripts/pr-hygiene.py`, `scripts/lib/gh_diff.py`, `scripts/hygiene/forbid-ledger.py` — at their
  own configured identity (e.g. `gh-tsouza`), without that identity's name ever being hardcoded in
  tracked source. Set it in the environment; it's read via `os.environ.get(...)`, never a literal.
- Git pushes need the GitHub noreply identity (`122435+tsouza@users.noreply.github.com`) — GitHub's GH007 rejects a real private email as author *or* committer.
- The `main` ruleset requires `required_status_checks` to be non-empty at creation time (a GitHub API constraint the plan didn't anticipate); it starts with no required-checks rule and gets one only via `make ruleset-add-check` once a real context has reported on `main` (T0.5).
- The org's built-in issue types are `Task`, `Bug`, `Feature`; `Epic` was added manually since the plan calls for it and `Feature` isn't the right shape for a milestone root.
- Our own Makefile targets never shadow one from the included `extension-ci-tools/makefiles/duckdb_extension.Makefile` (`release`, `debug`, `relassert`, `test`, `format`, `tidy-check`, …) — a same-named target here would silently override the upstream one for everyone, including the reusable CI workflows that invoke it directly by name. `test-relassert` exists as its own target only because upstream has no `test_relassert` pairing for the `relassert` build type; `tsan`/vector-size-2 need no target of their own at all — `THREADSAN=1 make debug` / `STANDARD_VECTOR_SIZE=2 make debug` then `make test_debug` reuse the upstream ones directly.
