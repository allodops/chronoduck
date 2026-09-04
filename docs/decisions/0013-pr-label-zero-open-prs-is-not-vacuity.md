---
status: accepted
date: 2026-09-04
deciders: tsouza
---

# pr-label backfill: zero open PRs is a legitimate empty state, not anti-vacuity failure

## Context

Issue #124's stated acceptance criterion for `scripts/pr-label.mjs`'s backfill mode was mined from
`../cerberus`'s equivalent script: "the script fails its run if it processes zero PRs... (anti-vacuity,
matching cerberus's pattern)." PR #125 implemented backfill mode to exit 0 on zero open PRs instead,
and its body claimed "Deviations: None" — an undisclosed deviation from that literal AC text. #125's
own fresh-session review (posted after merge) flagged the mismatch; #126 tracked fixing the record.

Cerberus's anti-vacuity guard makes sense there because cerberus always has open issues — a
"processed zero" result there is a real signal something upstream broke (e.g. the issues API call
silently returned nothing). Chronoduck's workflow is different: this repo routinely sits at zero open
PRs between work sessions (one issue claimed, branched, PR'd, reviewed and merged before the next is
claimed — see `AGENTS.md`'s "Working an issue" loop). Zero open PRs is the normal resting state, not
evidence of a broken API call.

## Decision

`scripts/pr-label.mjs`'s backfill mode exits 0 when `ghGetPaginated("/pulls", ...)` returns zero open
PRs. This does not weaken the anti-vacuity guarantee the AC was really after: `ghAddLabels`/`ghGet`
(`scripts/lib/gh.mjs`) throw on any API failure via Bun `$`'s default non-zero-exit-throws behavior,
uncaught, so a real API failure still fails the run loudly. The guarantee is structural (a thrown
error on failure), not a post-hoc "did we process at least one PR" count — the latter doesn't
distinguish "API broke" from "there was nothing to do," which is exactly the ambiguity that made
cerberus's literal guard the wrong fit here.

Issue #124's own acceptance-criterion text is now stale (it still reads "fails if it processes zero
PRs"); this ADR is the corrected record. The AC's actual intent — don't silently swallow a real
failure — is satisfied by the throw-on-failure behavior above, not by a zero-PRs-means-fail check.

## Consequences

- No code change: `scripts/pr-label.mjs` already behaves as decided here (confirmed on the PR #125
  branch as merged).
- A future reader of #124 should treat this ADR, not #124's original AC checkbox text, as the
  authoritative statement of intended backfill behavior on zero open PRs.
- PR bodies claiming "Deviations: None" must be checked against the issue's literal AC text, not just
  its Goal narrative — #125 is why: the code's actual (correct) behavior differed from the AC as
  written, and that difference went undisclosed.
