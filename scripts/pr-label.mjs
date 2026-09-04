#!/usr/bin/env bun
// make pr-label [PR=<n>]
// A PR should carry the size:/area: labels of the issue it closes — every
// issue is already labelled that way at creation (BOOTSTRAP §2.3), but
// nothing ever copied them onto the PR (confirmed against #112/#113: no PR
// in this repo has ever had a label). With PR set, labels that one PR
// (event mode, invoked by .github/workflows/pr-label.yml on open/edit/
// reopen); without it, walks every open PR (backfill mode) — additive and
// idempotent, never removes a label a human added by hand.
import { ghGet, ghGetPaginated, ghAddLabels } from "./lib/gh.mjs";

const LABEL_RE = /^(size|area):/;

export function closesIssueNumber(body) {
  const matches = [...(body || "").matchAll(/closes\s+#(\d+)/gi)];
  return matches.length === 1 ? Number(matches[0][1]) : null;
}

export function missingLabels(wanted, have) {
  const haveSet = new Set(have);
  return wanted.filter((l) => !haveSet.has(l));
}

async function fetchIssueLabels(number) {
  const issue = await ghGet(`/issues/${number}`);
  return (issue.labels || []).map((l) => l.name).filter((n) => LABEL_RE.test(n));
}

async function labelOne(pr) {
  if ((pr.user?.login || "").endsWith("[bot]")) {
    return { skipped: "bot" };
  }
  const issueNum = closesIssueNumber(pr.body);
  if (!issueNum) {
    return { skipped: "no-closes-link" };
  }
  const wanted = await fetchIssueLabels(issueNum);
  if (wanted.length === 0) {
    return { skipped: "issue-has-no-size/area-label" };
  }
  const have = (pr.labels || []).map((l) => l.name);
  const missing = missingLabels(wanted, have);
  if (missing.length === 0) {
    return { skipped: "already-labeled" };
  }
  await ghAddLabels(pr.number, missing);
  return { applied: missing };
}

// Guarded so importing this module for its pure functions (hygiene-selftest)
// never fires a real `gh api` call.
if (import.meta.main) {
  const prArg = process.argv.find((a) => a.startsWith("PR="))?.slice(3) || process.env.PR;

  if (prArg) {
    // Event mode: label exactly the one PR.
    const pr = await ghGet(`/pulls/${prArg}`);
    const result = await labelOne(pr);
    if (result.applied) {
      console.log(`pr-label: applied ${result.applied.join(", ")} to PR #${pr.number}`);
    } else {
      console.log(`pr-label: PR #${pr.number} — no-op (${result.skipped})`);
    }
  } else {
    // Backfill mode: walk every open PR, self-healing.
    const flat = await ghGetPaginated("/pulls", { state: "open" });
    if (flat.length === 0) {
      console.log("pr-label: backfill — no open PRs, nothing to do");
      process.exit(0);
    }
    // Anti-vacuity: labelOne() calls ghAddLabels() (which throws on API
    // failure, via Bun $'s default non-zero-exit-throws behavior) synchronously
    // before ever reporting "applied" — there is no code path that silently
    // swallows a label that should have been added. A thrown error here fails
    // the whole run loudly rather than producing a green no-op.
    let healed = 0;
    for (const pr of flat) {
      const result = await labelOne(pr);
      if (result.applied) {
        console.log(`pr-label: backfilled PR #${pr.number}: ${result.applied.join(", ")}`);
        healed++;
      }
    }
    console.log(`pr-label: backfill complete — ${flat.length} open PR(s), ${healed} healed`);
  }
}
