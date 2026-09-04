#!/usr/bin/env bun
import { $ } from "bun";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];
const baseIdx = args.indexOf("--base");
let base = baseIdx === -1 ? null : args[baseIdx + 1];

async function tryResolve(candidate) {
  try {
    await $`git -C ${root} rev-parse --verify ${candidate}`.quiet();
    return true;
  } catch {
    return false;
  }
}

async function hasMergeBase(candidate) {
  try {
    await $`git -C ${root} merge-base ${candidate} HEAD`.quiet();
    return true;
  } catch {
    return false;
  }
}

async function resolveBase() {
  if (base) return base;
  for (const candidate of ["origin/main", "main"]) {
    if (await tryResolve(candidate)) {
      base = candidate;
      break;
    }
  }
  if (!base) {
    // A PR-triggered CI checkout is typically shallow and only fetches the
    // ref needed for the merge commit — origin/main may genuinely not be a
    // resolvable local ref yet, not because there's no base to diff against.
    // Fetch it explicitly before concluding there's nothing to compare.
    try {
      await $`git -C ${root} fetch origin main:refs/remotes/origin/main`.quiet();
    } catch {
      // network/remote unavailable — fall through to "no base" below
    }
    if (await tryResolve("origin/main")) base = "origin/main";
  }
  if (!base) return null;

  // Three-dot diffing below (base...HEAD, relative to the merge-base) is what
  // actually answers "did *this branch* change CONSTITUTION.md" — a plain
  // two-dot diff against base's current tip would also pick up any unrelated
  // amendment landed on base after this branch forked, and falsely flag a
  // long-lived branch that never touched the file. But a shallow CI checkout
  // can resolve both refs individually while sharing no visible history, so
  // there may be no merge-base yet either — fix that specifically rather than
  // give up three-dot correctness for the common (non-shallow) case.
  if (!(await hasMergeBase(base))) {
    try {
      await $`git -C ${root} fetch --unshallow`.quiet();
    } catch {
      try {
        await $`git -C ${root} fetch --deepen=1000000`.quiet();
      } catch {
        // best effort — the diff below will fail closed if this didn't help
      }
    }
  }
  return base;
}

function versionOf(text) {
  const m = text.match(/\*\*Version\*\*\s+([\d.]+)/);
  return m ? m[1] : null;
}

function lastAmendedOf(text) {
  const m = text.match(/\*\*Last amended\*\*\s+([^\n*]+)/);
  return m ? m[1].trim() : null;
}

function versionGreater(a, b) {
  const pa = a.split(".").map(Number);
  const pb = b.split(".").map(Number);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const x = pa[i] ?? 0, y = pb[i] ?? 0;
    if (x !== y) return x > y;
  }
  return false;
}

// Splits CONSTITUTION.md text on top-level "## Article <id>" headers and
// returns a Map from article id (e.g. "II") to that section's full text
// (header line through the next "## Article" header or end of string). A
// plain text diff per section — no semantic understanding of what changed.
function articleSections(text) {
  const headerRe = /^## Article\s+(\S+)/gm;
  const matches = [...text.matchAll(headerRe)];
  const sections = new Map();
  for (let i = 0; i < matches.length; i++) {
    const start = matches[i].index;
    const end = i + 1 < matches.length ? matches[i + 1].index : text.length;
    sections.set(matches[i][1], text.slice(start, end));
  }
  return sections;
}

// Which article ids differ (by section text) between the old and new
// CONSTITUTION.md, including articles added or removed outright. Order:
// changed/new articles in their new-text order first, then any article
// removed outright (present in old, absent from new).
function changedArticles(oldText, newText) {
  const oldSections = articleSections(oldText);
  const newSections = articleSections(newText);
  const changed = [];
  for (const [id, body] of newSections) {
    if (oldSections.get(id) !== body) changed.push(id);
  }
  for (const id of oldSections.keys()) {
    if (!newSections.has(id)) changed.push(id);
  }
  return changed;
}

base = await resolveBase();
if (!base) {
  // Consistent with Article II.3's fail-closed stance: unable to verify means
  // red, never a silent pass — this check exists precisely to catch an
  // unamended constitution change, and a missing base ref can't rule that out.
  console.error("constitution-check: FAIL (no base ref to diff against — cannot verify CONSTITUTION.md)");
  process.exit(1);
}

let changedFiles;
try {
  // Three-dot: relative to the merge-base, so a change already on `base`
  // before this branch forked is never mistaken for something this PR did.
  changedFiles = (await $`git -C ${root} diff --name-only ${base}...HEAD`.text()).split("\n").filter(Boolean);
} catch {
  console.error(`constitution-check: FAIL (no merge-base between ${base} and HEAD even after attempting to fetch full history — cannot verify CONSTITUTION.md)`);
  process.exit(1);
}

if (!changedFiles.includes("CONSTITUTION.md")) {
  console.log("constitution-check: PASS (CONSTITUTION.md unchanged)");
  process.exit(0);
}

const violations = [];

let oldText = "";
try {
  oldText = await $`git -C ${root} show ${base}:CONSTITUTION.md`.text();
} catch {
  oldText = ""; // CONSTITUTION.md is new in this diff
}
let newText = "";
try {
  newText = await $`git -C ${root} show HEAD:CONSTITUTION.md`.text();
} catch {
  newText = await Bun.file(`${root}/CONSTITUTION.md`).text();
}

const oldVersion = versionOf(oldText);
const newVersion = versionOf(newText);
if (!newVersion || (oldVersion && !versionGreater(newVersion, oldVersion))) {
  violations.push(`CONSTITUTION.md changed but Version did not increase (${oldVersion ?? "none"} -> ${newVersion ?? "none"})`);
}

// Not compared against the old value: two amendments landing the same
// calendar day is legitimate (dates have no time component), and the
// version-bump check above is already the authoritative "did this actually
// get amended" signal — a version can't accidentally stay the same the way
// a same-day date can.
const newAmended = lastAmendedOf(newText);
if (!newAmended) {
  violations.push(`CONSTITUTION.md changed but has no Last amended date`);
}

const changedArticleIds = changedArticles(oldText, newText);

const addedAdrs = (
  await $`git -C ${root} diff --name-status ${base}...HEAD -- docs/decisions/`.text()
)
  .split("\n")
  .filter((l) => l.startsWith("A\t"))
  .map((l) => l.slice(2));

let hasAcceptedAdr = false;
let referencesChangedArticle = false;
for (const adr of addedAdrs) {
  const text = await $`git -C ${root} show HEAD:${adr}`.text();
  if (/status:\s*accepted/.test(text)) {
    hasAcceptedAdr = true;
    // \b after the id guards against a prefix collision between roman
    // numerals (e.g. "Article II" is a literal substring of "Article III") —
    // still a plain text match, not semantic understanding.
    if (changedArticleIds.some((id) => new RegExp(`Article\\s+${id}\\b`).test(text))) {
      referencesChangedArticle = true;
    }
  }
}
if (!hasAcceptedAdr) {
  // No accepted ADR exists at all — the pre-existing failure mode. Distinct
  // from (and reported instead of) the topicality violation below: with no
  // accepted ADR there is nothing whose body could reference a changed
  // article, so that check would have nothing to say.
  violations.push("CONSTITUTION.md changed but no new docs/decisions/*.md with `status: accepted` was added");
} else if (changedArticleIds.length > 0 && !referencesChangedArticle) {
  // An accepted ADR exists but its body is silent on every article that
  // actually changed — the "unrelated ADR attached" gap ACPR found. Zero
  // changed articles (e.g. only prose outside any "## Article" section
  // changed) has nothing to require here, so this only fires when the
  // section-level diff found at least one changed article id.
  violations.push(
    `CONSTITUTION.md changed Article(s) ${changedArticleIds.join(", ")} but no new accepted ADR references any of them`,
  );
}

if (violations.length > 0) {
  console.error("constitution-check: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("constitution-check: PASS");
