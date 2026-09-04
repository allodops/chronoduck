# Fresh-session PR review

Run this review in a fresh session with only the PR number and `CONSTITUTION.md` as context — no
memory of the implementing session. Post the review as a PR comment whose first line is exactly:

```
Fresh-session review:
```

Reject the PR (request changes) if any of the following hold. Each is a hard rejection, not a
style note — a PR failing one of these does not merge regardless of how green its checks are.

- **Out-of-scope files.** The diff touches a file the issue's stated scope does not imply
  (Article III.4). Discovered work belongs in a new linked issue, not in this diff.
- **Restated issue.** The PR body substantially repeats the issue body instead of describing
  execution — `make pr-hygiene PR=<n>` computes this as a shingle-overlap ratio over 0.15; treat any
  PR that reads like a paraphrase of its issue as a restatement even if the script hasn't run yet.
- **Missing evidence.** The PR body lacks a `## Evidence` line for one or more acceptance
  criteria, or a listed evidence line doesn't actually demonstrate the criterion.
- **Tolerances.** A new skip, soft assertion, tolerance file, allow-list or expected-failure set
  appears anywhere in the diff. The only sanctioned exclusion is a declared divergence in the
  comparator's closed enum (Article V.3).
- **Unregistered functions.** A new SQL-visible function exists outside `src/kernel/registry.def`
  once that file exists (Article V.1).
- **Line-number citations.** A code citation uses a line number instead of `file.cpp:function:` +
  a backticked expression that occurs exactly once in that file (Article II.4).

Two review rounds maximum per PR. A finding raised on a third round becomes a new issue linked
under the PR's Discovered section instead of blocking the merge further.
