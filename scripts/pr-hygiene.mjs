#!/usr/bin/env bun
import { $ } from "bun";
import { readFileSync, existsSync } from "node:fs";
import { join } from "node:path";
import { scanDiffForDeferral } from "./hygiene/forbid-deferral.mjs";

const args = process.argv.slice(2);
const fixtureIdx = args.indexOf("--fixture");

const REQUIRED_SECTIONS = ["## How", "## Deviations", "## Risk", "## Evidence", "## Discovered"];
const CONVENTIONAL_COMMITS_RE = /^(feat|fix|docs|style|refactor|perf|test|build|ci|chore|revert)(\([^)]+\))?!?: .+/;

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
      lastCommitAt: readIfExists(join(dir, "last-commit-at.txt")).trim() || null,
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
  const pr = await $`gh-tsouza pr view ${n} --json title,body,author,comments,commits`.json();
  const diff = await $`gh-tsouza pr diff ${n} --patch`.text();
  const commits = pr.commits ?? [];
  return {
    prTitle: pr.title,
    prBody: pr.body ?? "",
    prAuthor: pr.author?.login ?? "",
    diff,
    comments: pr.comments ?? [],
    lastCommitAt: commits.length > 0 ? commits[commits.length - 1].committedDate : null,
    loadIssue: async (issueNum) => {
      const issue = await $`gh-tsouza issue view ${issueNum} --json title,body`.json();
      return { issueTitle: issue.title, issueBody: issue.body ?? "" };
    },
  };
}

const FRESH_SESSION_REVIEW_PREFIX = "Fresh-session review:";

// Article VIII.2: "`make pr-hygiene` requires one dated after the PR's last
// commit." Enforces the review-before-merge order structurally instead of
// relying on a human/agent to eyeball comment vs. commit timestamps.
function freshSessionReviewViolation(comments, lastCommitAt) {
  const reviews = comments.filter((c) => c.body.startsWith(FRESH_SESSION_REVIEW_PREFIX));
  if (reviews.length === 0) {
    return `no "${FRESH_SESSION_REVIEW_PREFIX}" comment found (Article VIII.2)`;
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

const { prTitle, prBody, prAuthor, diff, comments, lastCommitAt, loadIssue } = await loadInputs();

// Article III.1: dependabot[bot] PRs are exempt from this article's body
// rules entirely — a version-bump PR has no "Closes #N", no design to
// describe, and nothing to restate. It still merges through the same
// green-checks loop as any other PR, just without this particular check.
if (prAuthor === "dependabot[bot]") {
  console.log("pr-hygiene: PASS (dependabot[bot] PR, exempt per Article III.1)");
  process.exit(0);
}

const violations = [];

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

if (!CONVENTIONAL_COMMITS_RE.test(prTitle)) {
  violations.push(`PR title "${prTitle}" does not match the Conventional Commits format`);
}

if (diff) {
  for (const v of scanDiffForDeferral(diff)) violations.push(v);
}

const reviewViolation = freshSessionReviewViolation(comments, lastCommitAt);
if (reviewViolation) violations.push(reviewViolation);

if (violations.length > 0) {
  console.error("pr-hygiene: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("pr-hygiene: PASS");
