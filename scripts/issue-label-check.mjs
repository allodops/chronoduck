#!/usr/bin/env bun
// make issue-label-check
// Every Task issue is labelled size:/area: at creation (BOOTSTRAP §2.2/2.3;
// an Epic is marked by its issue `type` instead and never carries these
// labels, so Epics are exempt here) — this is a drift check, not an
// inference tool: it flags any OPEN, non-Epic issue missing either label so
// it stays visible, rather than trying to guess a value that already has an
// authoritative source (the person filing the issue). Not a required check,
// not part of `make hygiene` — its own workflow on a schedule +
// workflow_dispatch (.github/workflows/issue-label-check.yml), deliberately
// red-until-fixed rather than silent.
//
// Runs standalone in CI, so plain `gh` + GH_TOKEN via scripts/lib/gh.mjs,
// same exception as scripts/pr-label.mjs.
import { ghGetPaginated } from "./lib/gh.mjs";

export function isMissingLabel(labelNames) {
  const has = (prefix) => labelNames.some((n) => n.startsWith(prefix));
  return !has("size:") || !has("area:");
}

// Guarded so importing this module for its pure functions (hygiene-selftest)
// never fires a real `gh api` call.
if (import.meta.main) {
  const issuesPages = await ghGetPaginated("/issues", { state: "open", filter: "all" });
  // Epics are never labelled size:/area: — BOOTSTRAP §2.2 labels only Tasks
  // that way; an Epic is marked by its issue `type`, not by a size/area pair.
  const issues = issuesPages.filter((i) => !("pull_request" in i) && i.type?.name !== "Epic");

  const flagged = issues.filter((i) => isMissingLabel((i.labels || []).map((l) => l.name)));

  if (process.env.GITHUB_STEP_SUMMARY) {
    const lines = ["## issue-label-check", ""];
    if (flagged.length === 0) {
      lines.push("All open issues carry both a `size:` and an `area:` label.");
    } else {
      lines.push(`${flagged.length} open issue(s) missing \`size:\` and/or \`area:\`:`, "");
      for (const i of flagged) lines.push(`- #${i.number} ${i.title}`);
    }
    await Bun.write(process.env.GITHUB_STEP_SUMMARY, lines.join("\n") + "\n");
  }

  if (flagged.length > 0) {
    console.error("issue-label-check: FAIL");
    for (const i of flagged) console.error(`  #${i.number} ${i.title}`);
    process.exit(1);
  }
  console.log(`issue-label-check: PASS (${issues.length} open issue(s), all labelled)`);
}
