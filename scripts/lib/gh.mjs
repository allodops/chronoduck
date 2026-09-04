// Shared plain-`gh`-CLI helper for scripts that run standalone in CI (no
// Claude Code session, no `gh-tsouza` alias available there) — the one
// deliberate exception to this project's "always gh-tsouza" rule, which is
// about interactive sessions on the owner's machine, not scripts GitHub
// Actions itself executes. Never call the gh*() functions below from a
// script meant to run interactively (scripts/ruleset.mjs is that case and
// keeps its own gh-tsouza constant; it is never invoked by a workflow, only
// by a human or Claude Code locally, per Article VII.3) — importing the
// REPO constant alone is fine, since it's just data, not a plain-`gh` call.
import { $ } from "bun";

export const REPO = "allodops/chronoduck";
const GH = "gh";

// A GET with query-string fields. `gh api` defaults to POST once any `-f`
// field is given, so `-X GET` must be explicit or it silently tries to
// create the resource instead of listing it.
export async function ghGetPaginated(path, fields = {}) {
  const fieldFlags = Object.entries(fields).flatMap(([k, v]) => ["-f", `${k}=${v}`]);
  const pages = await $`${GH} api -X GET repos/${REPO}${path} ${fieldFlags} --paginate --slurp`.json();
  return pages.flat();
}

export async function ghGet(path) {
  return await $`${GH} api repos/${REPO}${path}`.json();
}

export async function ghAddLabels(issueOrPrNumber, labels) {
  const flags = labels.flatMap((l) => ["-f", `labels[]=${l}`]);
  await $`${GH} api repos/${REPO}/issues/${issueOrPrNumber}/labels ${flags}`.quiet();
}
