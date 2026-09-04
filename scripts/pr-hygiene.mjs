#!/usr/bin/env bun
import { $ } from "bun";
import { readFileSync, existsSync } from "node:fs";
import { join } from "node:path";
import { scanDiffForDeferral } from "./hygiene/forbid-deferral.mjs";
import { fetchPrDiff } from "./lib/gh-diff.mjs";
import { CONVENTIONAL_COMMITS_RE } from "./lib/conventional-commits.mjs";

const args = process.argv.slice(2);
const fixtureIdx = args.indexOf("--fixture");

const REQUIRED_SECTIONS = ["## How", "## Deviations", "## Risk", "## Evidence", "## Discovered"];

function readIfExists(path) {
  return existsSync(path) ? readFileSync(path, "utf8") : "";
}

function readJSONIfExists(path, fallback) {
  return existsSync(path) ? JSON.parse(readFileSync(path, "utf8")) : fallback;
}

async function loadInputs() {
  if (fixtureIdx !== -1) {
    const dir = args[fixtureIdx + 1];
    return {
      prTitle: readIfExists(join(dir, "pr-title.txt")).trim(),
      prBody: readIfExists(join(dir, "pr-body.txt")),
      prAuthor: readIfExists(join(dir, "author.txt")).trim(),
      diff: readIfExists(join(dir, "diff.patch")),
      comments: readJSONIfExists(join(dir, "comments.json"), []),
      reviews: readJSONIfExists(join(dir, "reviews.json"), []),
      lastCommitAt: readIfExists(join(dir, "last-commit-at.txt")).trim() || null,
      mergedAt: readIfExists(join(dir, "merged-at.txt")).trim() || null,
      loadIssue: async (n) => ({
        issueTitle: readIfExists(join(dir, "issue-title.txt")).trim(),
        issueBody: readIfExists(join(dir, "issue-body.txt")),
      }),
    };
  }
  const n = args[0];
  if (!n || n.startsWith("--")) {
    console.error("pr-hygiene: usage: pr-hygiene.mjs <pr-number> | --fixture <dir>");
    process.exit(2);
  }
  const pr = await $`gh-tsouza pr view ${n} --json title,body,author,comments,commits,reviews,mergedAt`.json();
  // Net diff, not `--patch` (a per-commit patch series) — see
  // scripts/lib/gh-diff.mjs's fetchPrDiff() for the full rationale (#154).
  const diff = await fetchPrDiff(n);
  const commits = pr.commits ?? [];
  return {
    prTitle: pr.title,
    prBody: pr.body ?? "",
    prAuthor: pr.author?.login ?? "",
    diff,
    comments: pr.comments ?? [],
    reviews: pr.reviews ?? [],
    lastCommitAt: commits.length > 0 ? commits[commits.length - 1].committedDate : null,
    mergedAt: pr.mergedAt ?? null,
    loadIssue: async (issueNum) => {
      const issue = await $`gh-tsouza issue view ${issueNum} --json title,body`.json();
      return { issueTitle: issue.title, issueBody: issue.body ?? "" };
    },
  };
}

const FRESH_SESSION_REVIEW_PREFIX = "Fresh-session review:";

// Both plain issue comments and native `gh pr review` submissions can carry
// a "Fresh-session review:"-prefixed body (#166 AC: the check used to look
// only at `comments`, silently missing a review posted the native way —
// this repo's convention has always been plain comments, so it hadn't come
// up in practice, but nothing enforced that assumption). Normalized to a
// common {body, createdAt} shape so downstream logic doesn't care which
// kind it is.
function reviewCandidates(comments, reviews) {
  return [
    ...comments.map((c) => ({ body: c.body ?? "", createdAt: c.createdAt })),
    // A native review can be a bare state change (e.g. "Approve" clicked
    // with no written comment) — its body is then null, not "".
    ...reviews.map((r) => ({ body: r.body ?? "", createdAt: r.submittedAt })),
  ];
}

// Article VIII.2: "`make pr-hygiene` requires one dated after the PR's last
// commit." Enforces the review-before-merge order structurally instead of
// relying on a human/agent to eyeball comment vs. commit timestamps.
function freshSessionReviewViolation(candidates, lastCommitAt) {
  const reviews = candidates.filter((c) => c.body.startsWith(FRESH_SESSION_REVIEW_PREFIX));
  if (reviews.length === 0) {
    return `no "${FRESH_SESSION_REVIEW_PREFIX}" comment or review found (Article VIII.2)`;
  }
  if (!lastCommitAt) {
    return "no commit date available to check the review comment against";
  }
  const newest = reviews.reduce((a, b) => (new Date(a.createdAt) > new Date(b.createdAt) ? a : b));
  if (new Date(newest.createdAt) <= new Date(lastCommitAt)) {
    return `newest "${FRESH_SESSION_REVIEW_PREFIX}" comment (${newest.createdAt}) predates the PR's last commit (${lastCommitAt}) — Article VIII.2 requires one dated after`;
  }
  return null;
}

// Audit-only, not a merge-time gate — by the time this runs the PR is
// already merged, so this can only detect a "merged before its review
// completed" race after the fact, never prevent it (#166). Two real,
// historical instances of this exact failure mode predate this check:
// PR #112 and PR #125 (see test/hygiene-fixtures/pr-hygiene-merged-before-review-*),
// both reproduced with their real mergedAt/comment timestamps.
function mergeBeforeReviewViolation(mergedAt, candidates) {
  if (!mergedAt) return null; // not merged (yet) — nothing to audit
  const reviews = candidates.filter((c) => c.body.startsWith(FRESH_SESSION_REVIEW_PREFIX));
  if (reviews.length === 0) return null; // already reported by freshSessionReviewViolation
  const newest = reviews.reduce((a, b) => (new Date(a.createdAt) > new Date(b.createdAt) ? a : b));
  if (new Date(mergedAt) < new Date(newest.createdAt)) {
    return `PR merged at ${mergedAt}, before its newest "${FRESH_SESSION_REVIEW_PREFIX}" comment (${newest.createdAt}) — it merged before review completed (Article VIII.2 audit signal; this cannot be prevented after the fact, only caught)`;
  }
  return null;
}

function shingles8(text) {
  const words = text
    .toLowerCase()
    .replace(/\s+/g, " ")
    .trim()
    .split(" ")
    .filter(Boolean);
  const out = new Set();
  for (let i = 0; i + 8 <= words.length; i++) {
    out.add(words.slice(i, i + 8).join(" "));
  }
  return out;
}

function stripPrBodyForOverlap(body) {
  return body
    .split("\n")
    .filter((l) => !/^#{1,6}\s/.test(l) && !/^closes\s+#\d+/i.test(l))
    .join("\n");
}

const { prTitle, prBody, prAuthor, diff, comments, reviews, lastCommitAt, mergedAt, loadIssue } = await loadInputs();

const violations = [];

// Article III.1: Dependabot PRs are exempt from this article's *body* rules
// only — Closes #N, required sections, the Constitution-check line,
// title-vs-issue-title equality and the restated-issue shingle overlap.
// Article VIII.2 (the review gate) and the deferral scan are unconditional:
// nothing in Article III.1 carves either out, and a Dependabot-opened PR
// can still gain a human-authored commit after the fact (confirmed on
// PR #147, which received `fix: adapt to duckdb's Vector::Reference API
// change` as a second commit while `pr.author.login` stayed
// "app/dependabot" throughout) — #166. `gh pr view --json author` reports a
// real Dependabot PR's login as "app/dependabot" (not "dependabot[bot]", the
// git *commit author* string, a different field) — confirmed on #147/#148,
// real Dependabot-authored PRs (#162).
const isDependabot = prAuthor === "app/dependabot";

if (isDependabot) {
  console.log("pr-hygiene: Dependabot PR — Article III.1 body-rule checks (Closes#N, required sections, Constitution-check line, title-vs-issue-title, shingle overlap) skipped; deferral scan and Article VIII.2 review gate still apply.");
} else {
  const closesMatches = [...prBody.matchAll(/closes\s+#(\d+)/gi)];
  if (closesMatches.length !== 1) {
    violations.push(`expected exactly one "Closes #N", found ${closesMatches.length}`);
  }
  const issueNum = closesMatches[0]?.[1];

  for (const section of REQUIRED_SECTIONS) {
    if (!prBody.includes(section)) violations.push(`missing required section "${section}"`);
  }
  if (!/constitution check:/i.test(prBody)) {
    violations.push('missing required "Constitution check:" line');
  }

  if (issueNum) {
    const { issueTitle, issueBody } = await loadIssue(issueNum);

    if (issueTitle && prTitle.trim().toLowerCase() === issueTitle.trim().toLowerCase()) {
      violations.push("PR title must not equal the issue title");
    }

    const strippedPr = stripPrBodyForOverlap(prBody);
    const prShingles = shingles8(strippedPr);
    const issueShingles = shingles8(issueBody);
    let overlapCount = 0;
    for (const s of prShingles) if (issueShingles.has(s)) overlapCount++;
    const overlap = prShingles.size > 0 ? overlapCount / prShingles.size : 0;
    if (overlap > 0.15) {
      violations.push(`PR body overlaps issue body too closely (${(overlap * 100).toFixed(1)}% > 15%) — looks like a restated issue`);
    }
  }
}

// Not part of Article III.1's Dependabot exemption (only listed above), and
// real Dependabot PR titles already conform (e.g. "build(deps): bump ..."),
// so this runs unconditionally.
if (!CONVENTIONAL_COMMITS_RE.test(prTitle)) {
  violations.push(`PR title "${prTitle}" does not match the Conventional Commits format`);
}

if (diff) {
  for (const v of scanDiffForDeferral(diff)) violations.push(v);
}

const candidates = reviewCandidates(comments, reviews);

const reviewViolation = freshSessionReviewViolation(candidates, lastCommitAt);
if (reviewViolation) violations.push(reviewViolation);

const mergeViolation = mergeBeforeReviewViolation(mergedAt, candidates);
if (mergeViolation) violations.push(mergeViolation);

if (violations.length > 0) {
  console.error("pr-hygiene: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("pr-hygiene: PASS");
