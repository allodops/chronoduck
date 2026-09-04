// Shared `gh-tsouza pr diff <n>` fetch for interactive scripts run by a
// human/Claude Code on the owner's machine (never from a CI workflow — CI
// has no gh-tsouza identity installed; scripts/lib/gh.mjs's plain-`gh`
// helpers are the CI-safe equivalent). Used by both scripts/pr-hygiene.mjs
// and scripts/hygiene/forbid-deferral.mjs (#166 DRY: this fetch, including
// its rationale, used to be duplicated verbatim across both).
//
// Net diff, not `--patch` (a per-commit patch series) — the latter still
// shows a line as "added" in an early commit's patch even after a
// subsequent commit in the same PR removes it, false-positiving the
// deferral scan on something that never reaches the target branch (#154).
import { $ } from "bun";

export async function fetchPrDiff(n) {
  return await $`gh-tsouza pr diff ${n}`.text();
}
