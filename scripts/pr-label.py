#!/usr/bin/env python3
"""make pr-label [PR=<n>]

A PR should carry the size:/area: labels of the issue it closes -- every
issue is already labelled that way at creation (BOOTSTRAP §2.3), but
nothing ever copied them onto the PR (confirmed against #112/#113: no PR in
this repo has ever had a label). With PR set, labels that one PR (event
mode, invoked by .github/workflows/pr-label.yml on open/edit/reopen);
without it, walks every open PR (backfill mode) -- additive and idempotent,
never removes a label a human added by hand.
"""

import os
import re
import sys

from lib.gh import ghAddLabels, ghGet, ghGetPaginated

LABEL_RE = re.compile(r"^(size|area):")


def closesIssueNumber(body):
    matches = list(re.finditer(r"closes\s+#(\d+)", body or "", re.IGNORECASE))
    return int(matches[0].group(1)) if len(matches) == 1 else None


def missingLabels(wanted, have):
    have_set = set(have)
    return [l for l in wanted if l not in have_set]


def _fetch_issue_labels(number):
    issue = ghGet(f"/issues/{number}")
    return [l["name"] for l in (issue.get("labels") or []) if LABEL_RE.match(l["name"])]


def _label_one(pr):
    if (pr.get("user") or {}).get("login", "").endswith("[bot]"):
        return {"skipped": "bot"}
    issue_num = closesIssueNumber(pr.get("body"))
    if not issue_num:
        return {"skipped": "no-closes-link"}
    wanted = _fetch_issue_labels(issue_num)
    if not wanted:
        return {"skipped": "issue-has-no-size/area-label"}
    have = [l["name"] for l in (pr.get("labels") or [])]
    missing = missingLabels(wanted, have)
    if not missing:
        return {"skipped": "already-labeled"}
    ghAddLabels(pr["number"], missing)
    return {"applied": missing}


def main():
    pr_arg = None
    for a in sys.argv[1:]:
        if a.startswith("PR="):
            pr_arg = a[3:]
            break

    if not pr_arg:
        pr_arg = os.environ.get("PR")

    if pr_arg:
        # Event mode: label exactly the one PR.
        pr = ghGet(f"/pulls/{pr_arg}")
        result = _label_one(pr)
        if result.get("applied"):
            print(f"pr-label: applied {', '.join(result['applied'])} to PR #{pr['number']}")
        else:
            print(f"pr-label: PR #{pr['number']} — no-op ({result['skipped']})")
    else:
        # Backfill mode: walk every open PR, self-healing.
        flat = ghGetPaginated("/pulls", {"state": "open"})
        if not flat:
            print("pr-label: backfill — no open PRs, nothing to do")
            sys.exit(0)
        # Anti-vacuity: _label_one() calls ghAddLabels() (which raises on API
        # failure) synchronously before ever reporting "applied" -- there is
        # no code path that silently swallows a label that should have been
        # added. A raised error here fails the whole run loudly rather than
        # producing a green no-op.
        healed = 0
        for pr in flat:
            result = _label_one(pr)
            if result.get("applied"):
                print(f"pr-label: backfilled PR #{pr['number']}: {', '.join(result['applied'])}")
                healed += 1
        print(f"pr-label: backfill complete — {len(flat)} open PR(s), {healed} healed")


# Guarded so importing this module for its pure functions (hygiene-selftest)
# never fires a real `gh api` call.
if __name__ == "__main__":
    main()
