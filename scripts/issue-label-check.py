#!/usr/bin/env python3
"""make issue-label-check

Every Task issue is labelled size:/area: at creation (BOOTSTRAP §2.2/2.3; an Epic is marked by its issue
`type` instead and never carries these labels, so Epics are exempt here) --
this is a drift check, not an inference tool: it flags any OPEN, non-Epic
issue missing either label so it stays visible, rather than trying to guess
a value that already has an authoritative source (the person filing the
issue). Not a required check, not part of `make hygiene` -- its own
workflow on a schedule + workflow_dispatch
(.github/workflows/issue-label-check.yml), deliberately red-until-fixed
rather than silent.

Runs standalone in CI, so plain `gh` + GH_TOKEN via scripts/lib/gh.py, same
exception as scripts/pr-label.py.
"""

import os
import sys

from lib.gh import ghGetPaginated


def isMissingLabel(label_names):
    def has(prefix):
        return any(n.startswith(prefix) for n in label_names)

    return not has("size:") or not has("area:")


def main():
    issues_pages = ghGetPaginated("/issues", {"state": "open"})
    # Epics are never labelled size:/area: -- BOOTSTRAP §2.2 labels only Tasks
    # that way; an Epic is marked by its issue `type`, not by a size/area pair.
    issues = [
        i
        for i in issues_pages
        if "pull_request" not in i and (i.get("type") or {}).get("name") != "Epic"
    ]

    flagged = [i for i in issues if isMissingLabel([label["name"] for label in (i.get("labels") or [])])]

    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        lines = ["## issue-label-check", ""]
        if not flagged:
            lines.append("All open issues carry both a `size:` and an `area:` label.")
        else:
            lines.append(f"{len(flagged)} open issue(s) missing `size:` and/or `area:`:")
            lines.append("")
            for i in flagged:
                lines.append(f"- #{i['number']} {i['title']}")
        with open(step_summary, "w", encoding="utf8") as f:
            f.write("\n".join(lines) + "\n")

    if flagged:
        print("issue-label-check: FAIL", file=sys.stderr)
        for i in flagged:
            print(f"  #{i['number']} {i['title']}", file=sys.stderr)
        sys.exit(1)
    print(f"issue-label-check: PASS ({len(issues)} open issue(s), all labelled)")


# Guarded so importing this module for its pure functions (hygiene-selftest)
# never fires a real `gh api` call.
if __name__ == "__main__":
    main()
