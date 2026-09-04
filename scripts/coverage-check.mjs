#!/usr/bin/env bun
// make coverage-check
// Verifies docs/design/coverage.md's matrix still holds, per issue #25's
// acceptance criteria: (a) every K-disposition row names a real construct,
// (b) every milestone-shaped token cited in the matrix (a "(class N)"
// cross-reference, or a "T<major>.<minor>" plan-key) is real, (c) every K+
// row names a row or option now present, (d) every P row names a concrete
// planned construct.
//
// src/kernel/registry.def does not exist yet at M1 (Article V.1: "This
// article is enforced from the PR that creates registry.def"), so "names a
// real registry row" cannot mean "appears in registry.def". Per this issue,
// it instead means: the row's "How" column names a construct that appears in
// docs/design/primitives.md's Tier table or docs/design/architecture.md's
// state-class/edge-mode/value-domain vocabulary (an identifier verified
// against a real design doc), OR — since most K rows cite the *registry*
// function name itself (e.g. "rate", "ts_of_last_change_over_time"), which
// primitives.md deliberately does not enumerate (it lists the ~70 primitives
// registry rows compose from, not the ~115 rows themselves) — the row names
// at least one identifier-shaped, underscore-bearing token, tolerated as a
// forward reference to the not-yet-created registry.def. A row with neither
// (empty, or pure prose with no identifiable construct) fails.
//
// The "(class N)" self-reference check is fully offline. The "T<major>.<minor>"
// plan-key check shells out to `gh-tsouza issue list --search` to confirm the
// token names a real issue's `<!-- plan-key: T#.# -->` comment, mirroring
// forbid-ledger.mjs's issueIsOpen() pattern: when a token exists to verify and
// gh is unavailable, that is reported as a violation, not silently skipped
// (Article II.3's "offline runs report this check as red only when such a
// comment is present to verify" generalized to plan-key tokens).
import { $ } from "bun";
import { readFileSync, readdirSync, existsSync } from "node:fs";
import { join } from "node:path";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const DESIGN_DIR = join(root, "docs", "design");
const COVERAGE_PATH = join(DESIGN_DIR, "coverage.md");

if (!existsSync(COVERAGE_PATH)) {
  console.error("coverage-check: FAIL");
  console.error(`  ${COVERAGE_PATH} does not exist`);
  process.exit(1);
}

// -- vocabulary: identifiers named for real in the design docs -------------
//
// "The actual registry/kernel design docs" (issue #25) is every doc under
// docs/design/ other than coverage.md itself (primitives.md is the one the
// issue names explicitly, since it's the concrete stand-in for the
// not-yet-existing registry.def, but architecture.md's state classes/edge
// modes and schema.md's profile/role vocabulary are equally real designed
// constructs a K row can legitimately cite).
//
// Two extraction passes: backtick-quoted spans (split on non-identifier
// separators — spans are often compound: "hist_add / hist_sub",
// "Grid{start,end,step}", "reduce_fold<Op>") and heading words (a doc
// section titled "## The profile" makes "profile" a real, named concept even
// where the word itself is never wrapped in backticks). Matching is
// case-sensitive: coverage.md quotes edge modes/state classes in the same
// uppercase the design docs use, and lowercasing would make common English
// words in headings ("the", "and") collide with prose everywhere.
const STOPWORDS = new Set(["left", "right", "count", "first", "last", "any", "new", "one", "two", "t", "v", "st", "ts", "the", "and", "for", "with"]);

function identifiersFromBacktickSpans(text) {
  const out = new Set();
  for (const m of text.matchAll(/`([^`]+)`/g)) {
    for (const tok of m[1].split(/[^A-Za-z0-9_]+/)) {
      if (tok.length < 3 || STOPWORDS.has(tok.toLowerCase())) continue;
      out.add(tok);
    }
  }
  return out;
}

function identifiersFromHeadings(text) {
  const out = new Set();
  for (const line of text.split("\n")) {
    if (!line.startsWith("#")) continue;
    for (const tok of line.replace(/^#+\s*/, "").split(/[^A-Za-z0-9_]+/)) {
      if (tok.length < 4 || STOPWORDS.has(tok.toLowerCase())) continue;
      out.add(tok);
    }
  }
  return out;
}

const designVocab = new Set();
if (existsSync(DESIGN_DIR)) {
  for (const entry of readdirSync(DESIGN_DIR)) {
    if (!entry.endsWith(".md") || entry === "coverage.md") continue;
    const text = readFileSync(join(DESIGN_DIR, entry), "utf8");
    for (const tok of identifiersFromBacktickSpans(text)) designVocab.add(tok);
    for (const tok of identifiersFromHeadings(text)) designVocab.add(tok);
  }
}

// An identifier-shaped, underscore-bearing token — the shape of a registry
// function name (rate_over_time, hist_detect_reset, ts_of_last_change_over_time)
// not yet in registry.def, tolerated as a forward reference. No trailing
// word-boundary assertion: a row's How column can end a token with a suffix
// like "series_time_decayed_*" (a wildcard family name), where "_" keeps
// \b from firing right after the identifier's last letter.
const FORWARD_REF_RE = /\b[a-z][a-z0-9]*_[a-z0-9]+/;

function namesRealConstruct(text) {
  for (const vocabTerm of designVocab) {
    // word-boundary-ish containment check, case-sensitive
    if (text.includes(vocabTerm)) return vocabTerm;
  }
  return null;
}

// -- parse coverage.md into class sections and table rows ------------------

const coverageText = readFileSync(COVERAGE_PATH, "utf8");
const lines = coverageText.split("\n");

const classHeadings = new Set(); // every "## N. Title" number
const rows = []; // {classNum, primitive, appearsIn, where, how, lineNo}
let currentClass = null;

for (let i = 0; i < lines.length; i++) {
  const line = lines[i];
  const headingMatch = line.match(/^## (\d+)\.\s/);
  if (headingMatch) {
    currentClass = Number(headingMatch[1]);
    classHeadings.add(currentClass);
    continue;
  }
  if (!line.startsWith("|")) continue;
  if (/^\|\s*Primitive\s*\|/.test(line)) continue; // header row
  if (/^\|[-\s|]+\|$/.test(line)) continue; // separator row
  const cells = line
    .split("|")
    .slice(1, -1)
    .map((c) => c.trim());
  if (cells.length !== 4) continue; // not a data row of this table shape
  const [primitive, appearsIn, where, how] = cells;
  if (primitive === "" && appearsIn === "" && where === "" && how === "") continue;
  rows.push({ classNum: currentClass, primitive, appearsIn, where, how, lineNo: i + 1 });
}

const violations = [];
let verifiedCount = 0;
let toleratedCount = 0;

// (a) + (c): every K and K+ row names a real construct or, failing that, a
// forward-reference-shaped identifier.
for (const row of rows) {
  if (row.where !== "K" && row.where !== "K+") continue;
  const haystack = `${row.primitive} ${row.how}`;
  const hit = namesRealConstruct(haystack);
  if (hit) {
    verifiedCount++;
    continue;
  }
  if (FORWARD_REF_RE.test(haystack)) {
    toleratedCount++;
    continue;
  }
  violations.push(
    `coverage.md:${row.lineNo}: ${row.where} row "${row.primitive}" (class ${row.classNum}) names no identifiable construct in its "How" column — not in docs/design/primitives.md, docs/design/architecture.md, and no registry-function-shaped identifier either`,
  );
}

// (d): every P row names a concrete planned construct (non-trivial "How").
for (const row of rows) {
  if (row.where !== "P") continue;
  const haystack = `${row.primitive} ${row.how}`;
  if (!FORWARD_REF_RE.test(haystack) && !namesRealConstruct(haystack)) {
    violations.push(`coverage.md:${row.lineNo}: P row "${row.primitive}" (class ${row.classNum}) names no concrete planned construct in its "How" column`);
  }
}

// (b) part 1: every "(class N)" cross-reference resolves to a real heading.
for (const row of rows) {
  const haystack = `${row.appearsIn} ${row.how}`;
  for (const m of haystack.matchAll(/\bclass (\d+)\b/g)) {
    const n = Number(m[1]);
    if (!classHeadings.has(n)) {
      violations.push(`coverage.md:${row.lineNo}: "${row.primitive}" cites "class ${n}", which is not a heading in this document`);
    }
  }
}

// (b) part 2: every "T<major>.<minor>" plan-key token names a real issue.
const planKeyTokens = new Set([...coverageText.matchAll(/\bT\d+\.\d+\b/g)].map((m) => m[0]));

async function issueExistsForPlanKey(token) {
  try {
    const out = await $`gh-tsouza issue list --state all --search ${`"plan-key: ${token}" in:body`} --json number`.text();
    const parsed = JSON.parse(out);
    return Array.isArray(parsed) && parsed.length > 0;
  } catch {
    return null; // gh unavailable
  }
}

for (const token of planKeyTokens) {
  const exists = await issueExistsForPlanKey(token);
  if (exists === null) {
    violations.push(`coverage.md: could not verify plan-key "${token}" (gh-tsouza unavailable)`);
  } else if (!exists) {
    violations.push(`coverage.md: cites plan-key "${token}", which no issue's "<!-- plan-key: ${token} -->" comment names`);
  }
}

if (violations.length > 0) {
  console.error("coverage-check: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log(
  `coverage-check: PASS (${rows.length} row(s); ${verifiedCount} K/K+ row(s) verified against a real construct, ${toleratedCount} tolerated as forward references to registry.def; ${classHeadings.size} class heading(s); ${planKeyTokens.size} plan-key token(s) verified)`,
);
