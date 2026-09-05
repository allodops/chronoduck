#!/usr/bin/env bun
// make tier-coverage-floor — issue #36's fourth meta-test: "a per-tier
// coverage floor, raise-only, refusing zero." `docs/testing/primitives.md`
// tables every primitive `docs/testing/layers.md`'s L1a row requires "its
// own translation unit, its own table-driven tests ... exercised directly"
// for, grouped into Tier 0-6; `test/kernel/*_test.cpp`'s own header
// comments already cite which Tier/primitive each file directly tests
// (e.g. `kahan_test.cpp`: "Structure follows `docs/testing/primitives.md`'s
// Tier 0 `kahan_add / kahan_merge` row") — this scan reads that citation
// convention as data rather than adding a second, parallel manifest.
//
// Coverage per tier = |primitives in that tier's table cited by some
// test/kernel/*_test.cpp| / |primitives in that tier's table|. The floor is
// `test/kernel/tier-coverage-floor.json`, a hand-maintained ratchet keyed by
// "Tier N", holding the covered *count* (not the fraction — Article V.4's
// "ratchets gate on identity, never counts" governs the *fixture-identity*
// rosters (T7); this is a distinct, count-shaped ratchet the issue's own
// title names separately: "coverage floors").
//
// Three fatal outcomes, mirroring the REGRESSED/UNRECORDED shape
// `scripts/hygiene/kernel-fixture-loader.mjs` established for identity
// ratchets, adapted to counts:
//   - REGRESSED: a tracked tier's current coverage fell below its floor.
//   - STALE: a tracked tier's current coverage rose past its floor without
//     the floor file being raised to match — "raise-only" means the raise
//     is a reviewed, same-PR edit, never automatic and never silently
//     behind reality.
//   - UNTRACKED: an untracked tier now has nonzero coverage and needs an
//     entry.
// A fourth, ZERO-FLOOR, is the issue's own "refusing zero": a floor entry of
// 0 asserts nothing (any coverage is >= 0), so it is refused outright rather
// than tolerated as a currently-harmless no-op — a tier with no coverage
// yet simply has no entry at all, matching every other roster in this repo
// (an unstarted fixture, primitive or lane is absent, never present at 0).
//
// A fifth, non-ratchet check catches the citations themselves rotting: a
// test file citing "Tier N `<X>` row" where X is not literally one of Tier
// N's primitive names in docs/testing/primitives.md today (a typo, or a
// primitive renamed in the doc without its test citation following) is
// flagged by name — the same "the test suite rotting" concern L13's own row
// in docs/testing/layers.md names as this layer's whole point.
import { readFileSync, readdirSync, existsSync } from "node:fs";
import { join } from "node:path";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const PRIMITIVES_MD = join(root, "docs", "testing", "primitives.md");
const KERNEL_TEST_DIR = join(root, "test", "kernel");
const FLOOR_PATH = join(root, "test", "kernel", "tier-coverage-floor.json");

// -- parse docs/testing/primitives.md's Tier tables -------------------------

function parseTierPrimitives(text) {
  const tiers = new Map(); // tier number -> [primitive name, ...]
  let current = null;
  for (const line of text.split("\n")) {
    const heading = line.match(/^## Tier (\d+)\b/);
    if (heading) {
      current = Number(heading[1]);
      tiers.set(current, []);
      continue;
    }
    if (line.match(/^##\s/) && !heading) {
      current = null; // left the Tier sections entirely (e.g. "## Integration seams")
      continue;
    }
    if (current === null || !line.startsWith("|")) continue;
    if (/^\|\s*Primitive\s*\|/.test(line)) continue; // header row
    if (/^\|[-\s|]+\|$/.test(line)) continue; // separator row
    const firstCell = line.split("|")[1]?.trim();
    if (firstCell) tiers.get(current).push(firstCell);
  }
  return tiers;
}

if (!existsSync(PRIMITIVES_MD)) {
  console.error("tier-coverage-floor: FAIL");
  console.error(`  ${PRIMITIVES_MD} does not exist`);
  process.exit(1);
}
const tierPrimitives = parseTierPrimitives(readFileSync(PRIMITIVES_MD, "utf8"));

// -- parse test/kernel/*_test.cpp's citation comments ------------------------

// A contiguous run of `//` lines, marker and one leading space stripped,
// joined with a single space — the same wrap-tolerant approach
// `scripts/hygiene/verify-citations.mjs` uses for citations split across a
// clang-format/prettier comment reflow, applied here to the Tier citation
// instead.
function joinedCommentBlock(content) {
  const lines = content.split("\n");
  const parts = [];
  for (const line of lines) {
    const m = line.match(/^\s*\/\/\s?(.*)$/);
    if (!m) break; // header comment ends at the first non-// line
    parts.push(m[1]);
  }
  return parts.join(" ");
}

// "Tier N `a` row" or "Tier N `a` and `b` rows" — one or more backtick
// spans, joined by "and"/"," and whitespace, between "Tier N" and the
// literal word "row"/"rows".
const TIER_CITATION_RE = /Tier (\d+)((?:\s*(?:,|and)?\s*`[^`]+`)+)\s+rows?\b/g;
const BACKTICK_RE = /`([^`]+)`/g;

function parseCitations(joined) {
  const citations = []; // { tier, primitive }
  let m;
  TIER_CITATION_RE.lastIndex = 0;
  while ((m = TIER_CITATION_RE.exec(joined))) {
    const tier = Number(m[1]);
    BACKTICK_RE.lastIndex = 0;
    let bm;
    while ((bm = BACKTICK_RE.exec(m[2]))) {
      citations.push({ tier, primitive: bm[1] });
    }
  }
  return citations;
}

let testFiles;
try {
  testFiles = readdirSync(KERNEL_TEST_DIR).filter((f) => f.endsWith("_test.cpp"));
} catch {
  testFiles = [];
}

const covered = new Map(); // tier -> Set(primitive)
const unknownCitations = []; // { file, tier, primitive }

for (const file of testFiles) {
  const content = readFileSync(join(KERNEL_TEST_DIR, file), "utf8");
  const joined = joinedCommentBlock(content);
  for (const { tier, primitive } of parseCitations(joined)) {
    const known = tierPrimitives.get(tier) ?? [];
    if (!known.includes(primitive)) {
      unknownCitations.push({ file, tier, primitive });
      continue;
    }
    if (!covered.has(tier)) covered.set(tier, new Set());
    covered.get(tier).add(primitive);
  }
}

// -- compare against the committed floor -------------------------------------

const floor = existsSync(FLOOR_PATH) ? JSON.parse(readFileSync(FLOOR_PATH, "utf8")) : {};
const violations = [];

for (const [key, floorCount] of Object.entries(floor)) {
  const tier = Number(key.replace(/^Tier /, ""));
  const currentCount = (covered.get(tier) ?? new Set()).size;
  if (floorCount === 0) {
    violations.push(`ZERO-FLOOR: "${key}" is recorded at 0 in ${relFloorPath()} — a zero floor asserts nothing; remove the entry instead until the tier has real coverage`);
    continue;
  }
  if (currentCount < floorCount) {
    violations.push(`REGRESSED: "${key}" covers ${currentCount} primitive(s) now, below its committed floor of ${floorCount}`);
  } else if (currentCount > floorCount) {
    violations.push(`STALE: "${key}" covers ${currentCount} primitive(s) now, above its committed floor of ${floorCount} — raise ${relFloorPath()} to ${currentCount}`);
  }
}

for (const [tier, names] of covered) {
  const key = `Tier ${tier}`;
  if (!(key in floor) && names.size > 0) {
    violations.push(`UNTRACKED: "${key}" now covers ${names.size} primitive(s) (${[...names].join(", ")}) but has no entry in ${relFloorPath()} — add "${key}": ${names.size}`);
  }
}

for (const { file, tier, primitive } of unknownCitations) {
  violations.push(`UNKNOWN-PRIMITIVE: test/kernel/${file} cites Tier ${tier} \`${primitive}\` row, which is not a primitive docs/testing/primitives.md's Tier ${tier} table lists`);
}

function relFloorPath() {
  return "test/kernel/tier-coverage-floor.json";
}

if (violations.length > 0) {
  console.error("tier-coverage-floor: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log(`tier-coverage-floor: PASS (${Object.keys(floor).length} tracked tier(s))`);
