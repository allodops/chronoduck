#!/usr/bin/env python3
"""make pr-hygiene PR=<n>

Scans an open (or merged) PR against Article III/VIII's rules: exactly one
"Closes #N", the required PR-body sections, a "Constitution check:" line, a
Conventional-Commits title, no deferral language without an issue reference
in the diff, and Article VIII.2's fresh-session-review-postdates-last-commit
gate.

Run interactively, on the owner's machine (never by a CI workflow), so this
fetches via the configurable interactive GitHub CLI directly
(scripts/lib/gh_diff.py's fetchPrDiff, and its own interactive-CLI
subprocess calls below) rather than scripts/lib/gh.py's plain-`gh` helpers,
which are the CI-safe equivalent for standalone scripts run without an
interactively-configured GitHub CLI identity.
"""

import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

from lib.conventional_commits import CONVENTIONAL_COMMITS_RE
from lib.gh_diff import fetchPrDiff

# scripts/hygiene.py (this repo's hygiene orchestrator) and scripts/hygiene/
# (the scan directory) share the name "hygiene" -- Python's import system
# resolves a same-directory "hygiene.py" module ahead of a "hygiene/"
# namespace package for that name, so `from hygiene.forbid_deferral import
# scanDiffForDeferral` would find the wrong one. Importing directly from
# scripts/hygiene/ (added to sys.path on its own, ahead of the
# module-vs-package collision) sidesteps that, the same way
# scripts/hygiene-selftest.py already does (#241).
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "hygiene"))
from forbid_deferral import scanDiffForDeferral  # noqa: E402

# The interactive GitHub CLI identity: configured per-operator via the
# environment, outside tracked source, so no personal alias is ever a literal
# in this file. Defaults to plain `gh` for anyone without one configured.
GH_INTERACTIVE = os.environ.get("CHRONODUCK_GH_INTERACTIVE_CLI", "gh")

REQUIRED_SECTIONS = ["## How", "## Deviations", "## Risk", "## Evidence", "## Discovered"]

FRESH_SESSION_REVIEW_PREFIX = "Fresh-session review:"


# ---------------------------------------------------------------------------
# Input loading -- fixture mode (test/hygiene-fixtures/<dir>) or live PR mode.
# ---------------------------------------------------------------------------


def _read_if_exists(path):
    return path.read_text(encoding="utf8") if path.exists() else ""


def _read_json_if_exists(path, fallback):
    return json.loads(path.read_text(encoding="utf8")) if path.exists() else fallback


def _gh_json(args):
    result = subprocess.run([GH_INTERACTIVE, *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"{GH_INTERACTIVE} {' '.join(args)} failed (exit {result.returncode}): {result.stderr.strip()}")
    return json.loads(result.stdout)


def load_inputs(args):
    if "--fixture" in args:
        d = Path(args[args.index("--fixture") + 1])

        def load_issue(_n):
            return {
                "issueTitle": _read_if_exists(d / "issue-title.txt").strip(),
                "issueBody": _read_if_exists(d / "issue-body.txt"),
            }

        return {
            "prTitle": _read_if_exists(d / "pr-title.txt").strip(),
            "prBody": _read_if_exists(d / "pr-body.txt"),
            "prAuthor": _read_if_exists(d / "author.txt").strip(),
            "diff": _read_if_exists(d / "diff.patch"),
            "comments": _read_json_if_exists(d / "comments.json", []),
            "reviews": _read_json_if_exists(d / "reviews.json", []),
            "lastCommitAt": _read_if_exists(d / "last-commit-at.txt").strip() or None,
            "mergedAt": _read_if_exists(d / "merged-at.txt").strip() or None,
            "loadIssue": load_issue,
        }

    n = args[0] if args and not args[0].startswith("--") else None
    if not n:
        print("pr-hygiene: usage: pr-hygiene.py <pr-number> | --fixture <dir>", file=sys.stderr)
        sys.exit(2)

    pr = _gh_json(["pr", "view", n, "--json", "title,body,author,comments,commits,reviews,mergedAt"])
    # Net diff, not `--patch` (a per-commit patch series) -- see
    # scripts/lib/gh_diff.py's fetchPrDiff() for the full rationale (#154).
    diff = fetchPrDiff(n)
    commits = pr.get("commits") or []

    def load_issue(issue_num):
        issue = _gh_json(["issue", "view", str(issue_num), "--json", "title,body"])
        return {"issueTitle": issue["title"], "issueBody": issue.get("body") or ""}

    return {
        "prTitle": pr["title"],
        "prBody": pr.get("body") or "",
        "prAuthor": (pr.get("author") or {}).get("login") or "",
        "diff": diff,
        "comments": pr.get("comments") or [],
        "reviews": pr.get("reviews") or [],
        "lastCommitAt": commits[-1]["committedDate"] if commits else None,
        "mergedAt": pr.get("mergedAt"),
        "loadIssue": load_issue,
    }


# ---------------------------------------------------------------------------
# Review-gate checks (Article VIII.2).
# ---------------------------------------------------------------------------


# Both plain issue comments and native `gh pr review` submissions can carry
# a "Fresh-session review:"-prefixed body (#166 AC: the check used to look
# only at `comments`, silently missing a review posted the native way --
# this repo's convention has always been plain comments, so it hadn't come
# up in practice, but nothing enforced that assumption). Normalized to a
# common {body, createdAt} shape so downstream logic doesn't care which
# kind it is.
def reviewCandidates(comments, reviews):
    out = [{"body": c.get("body") or "", "createdAt": c.get("createdAt")} for c in comments]
    # A native review can be a bare state change (e.g. "Approve" clicked
    # with no written comment) -- its body is then null, not "".
    out += [{"body": r.get("body") or "", "createdAt": r.get("submittedAt")} for r in reviews]
    return out


def _parse_date(s):
    return datetime.fromisoformat(str(s).replace("Z", "+00:00"))


# Article VIII.2: "`make pr-hygiene` requires one dated after the PR's last
# commit." Enforces the review-before-merge order structurally instead of
# relying on a human/agent to eyeball comment vs. commit timestamps.
def freshSessionReviewViolation(candidates, last_commit_at):
    reviews = [c for c in candidates if c["body"].startswith(FRESH_SESSION_REVIEW_PREFIX)]
    if not reviews:
        return f'no "{FRESH_SESSION_REVIEW_PREFIX}" comment or review found (Article VIII.2)'
    if not last_commit_at:
        return "no commit date available to check the review comment against"
    newest = max(reviews, key=lambda c: _parse_date(c["createdAt"]))
    if _parse_date(newest["createdAt"]) <= _parse_date(last_commit_at):
        return (
            f'newest "{FRESH_SESSION_REVIEW_PREFIX}" comment ({newest["createdAt"]}) predates the PR\'s last commit '
            f"({last_commit_at}) — Article VIII.2 requires one dated after"
        )
    return None


# Audit-only, not a merge-time gate -- by the time this runs the PR is
# already merged, so this can only detect a "merged before its review
# completed" race after the fact, never prevent it (#166). Two real,
# historical instances of this exact failure mode predate this check:
# PR #112 and PR #125 (see test/hygiene-fixtures/pr-hygiene-merged-before-review-*),
# both reproduced with their real mergedAt/comment timestamps.
def mergeBeforeReviewViolation(merged_at, candidates):
    if not merged_at:
        return None  # not merged (yet) -- nothing to audit
    reviews = [c for c in candidates if c["body"].startswith(FRESH_SESSION_REVIEW_PREFIX)]
    if not reviews:
        return None  # already reported by freshSessionReviewViolation
    newest = max(reviews, key=lambda c: _parse_date(c["createdAt"]))
    if _parse_date(merged_at) < _parse_date(newest["createdAt"]):
        return (
            f"PR merged at {merged_at}, before its newest \"{FRESH_SESSION_REVIEW_PREFIX}\" comment "
            f'({newest["createdAt"]}) — it merged before review completed (Article VIII.2 audit signal; '
            "this cannot be prevented after the fact, only caught)"
        )
    return None


def shingles8(text):
    words = [w for w in re.sub(r"\s+", " ", text.lower()).strip().split(" ") if w]
    out = set()
    for i in range(0, len(words) - 7):
        out.add(" ".join(words[i : i + 8]))
    return out


def stripPrBodyForOverlap(body):
    return "\n".join(
        line
        for line in body.split("\n")
        if not re.match(r"^#{1,6}\s", line) and not re.match(r"^closes\s+#\d+", line, re.IGNORECASE)
    )


def main():
    args = sys.argv[1:]
    inputs = load_inputs(args)
    pr_title = inputs["prTitle"]
    pr_body = inputs["prBody"]
    pr_author = inputs["prAuthor"]
    diff = inputs["diff"]
    comments = inputs["comments"]
    reviews = inputs["reviews"]
    last_commit_at = inputs["lastCommitAt"]
    merged_at = inputs["mergedAt"]
    load_issue = inputs["loadIssue"]

    violations = []

    # Article III.1: Dependabot PRs are exempt from this article's *body* rules
    # only -- Closes #N, required sections, the Constitution-check line,
    # title-vs-issue-title equality and the restated-issue shingle overlap.
    # Article VIII.2 (the review gate) and the deferral scan are unconditional:
    # nothing in Article III.1 carves either out, and a Dependabot-opened PR
    # can still gain a human-authored commit after the fact (confirmed on
    # PR #147, which received `fix: adapt to duckdb's Vector::Reference API
    # change` as a second commit while `pr.author.login` stayed
    # "app/dependabot" throughout) -- #166. `gh pr view --json author` reports a
    # real Dependabot PR's login as "app/dependabot" (not "dependabot[bot]", the
    # git *commit author* string, a different field) -- confirmed on #147/#148,
    # real Dependabot-authored PRs (#162).
    is_dependabot = pr_author == "app/dependabot"

    if is_dependabot:
        print(
            "pr-hygiene: Dependabot PR — Article III.1 body-rule checks (Closes#N, required sections, "
            "Constitution-check line, title-vs-issue-title, shingle overlap) skipped; deferral scan and "
            "Article VIII.2 review gate still apply."
        )
    else:
        closes_matches = list(re.finditer(r"closes\s+#(\d+)", pr_body, re.IGNORECASE))
        if len(closes_matches) != 1:
            violations.append(f'expected exactly one "Closes #N", found {len(closes_matches)}')
        issue_num = closes_matches[0].group(1) if closes_matches else None

        for section in REQUIRED_SECTIONS:
            if section not in pr_body:
                violations.append(f'missing required section "{section}"')
        if not re.search(r"constitution check:", pr_body, re.IGNORECASE):
            violations.append('missing required "Constitution check:" line')

        if issue_num:
            issue = load_issue(issue_num)
            issue_title = issue["issueTitle"]
            issue_body = issue["issueBody"]

            if issue_title and pr_title.strip().lower() == issue_title.strip().lower():
                violations.append("PR title must not equal the issue title")

            stripped_pr = stripPrBodyForOverlap(pr_body)
            pr_shingles = shingles8(stripped_pr)
            issue_shingles = shingles8(issue_body)
            overlap_count = sum(1 for s in pr_shingles if s in issue_shingles)
            overlap = overlap_count / len(pr_shingles) if pr_shingles else 0
            if overlap > 0.15:
                violations.append(
                    f"PR body overlaps issue body too closely ({overlap * 100:.1f}% > 15%) — looks like a restated issue"
                )

    # Not part of Article III.1's Dependabot exemption (only listed above), and
    # real Dependabot PR titles already conform (e.g. "build(deps): bump ..."),
    # so this runs unconditionally.
    if not CONVENTIONAL_COMMITS_RE.match(pr_title):
        violations.append(f'PR title "{pr_title}" does not match the Conventional Commits format')

    if diff:
        violations.extend(scanDiffForDeferral(diff))

    candidates = reviewCandidates(comments, reviews)

    review_violation = freshSessionReviewViolation(candidates, last_commit_at)
    if review_violation:
        violations.append(review_violation)

    merge_violation = mergeBeforeReviewViolation(merged_at, candidates)
    if merge_violation:
        violations.append(merge_violation)

    if violations:
        print("pr-hygiene: FAIL", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        sys.exit(1)
    print("pr-hygiene: PASS")


if __name__ == "__main__":
    main()
