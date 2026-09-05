#!/usr/bin/env bun
// make divergence-enum-coverage — issue #36's third meta-test: "divergence
// enum values no fixture exercises" (Article V.3: "The only sanctioned
// exclusion is a declared divergence in the comparator's closed enum; an
// enum value no fixture exercises is a failure."; T13 and
// `docs/testing/live-oracles.md`'s "Declared divergences" section: "the L13
// meta-test fails if an enum value is never exercised by any fixture — an
// unused divergence is a tolerance waiting to be used.").
//
// `docs/testing/live-oracles.md` shows the closed enums this rule will
// eventually govern (`ChdbDivergence`, `TimescaleDivergence`,
// `ChplanDivergence`) as a design sketch for the L6a live-oracle harness —
// which is not part of M1 (`docs/testing/layers.md`'s L6a row is [MERGE]
// for chDB and [RELEASE] for Timescale, and M1's own milestone description
// lists only L0, L1a/b, L2, L4, L5, L13). Building that harness is out of
// this issue's scope. What this scan fences instead is the mechanism
// itself, against wherever a real `enum class *Divergence` actually lives
// in the kernel's own source — so the rule is already live and enforced the
// day a future issue adds the first one, rather than a second thing that
// issue would also need to remember to wire up. Today `src/` declares no
// such enum, so this scan is vacuously green on the real tree; its fixture
// branch proves the mechanism on a synthetic one (Article V.5 still needs a
// real, provably-executed lane — `scripts/hygiene.mjs`'s TREE_SCANS — not a
// hope that a future issue remembers this file exists).
import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { join } from "node:path";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const SRC_DIR = join(root, "src");
const FIXTURES_DIR = join(root, "test", "fixtures");
const SQL_TEST_DIR = join(root, "test", "sql");

function listFiles(dir, pred) {
  if (!existsSync(dir)) return [];
  const out = [];
  const walk = (d) => {
    for (const entry of readdirSync(d)) {
      const full = join(d, entry);
      const st = statSync(full);
      if (st.isDirectory()) walk(full);
      else if (pred(entry)) out.push(full);
    }
  };
  walk(dir);
  return out;
}

// `enum class <Name>Divergence { ... }` — a flat, comment-bearing C++ enum,
// the shape `docs/testing/live-oracles.md`'s own shown block uses. No
// nested braces are expected inside a declared-divergence enum body, so a
// non-greedy match up to the first `}` is sufficient (the same
// "regex scan, not a real parser" posture `registry-closure.mjs` documents).
const DIVERGENCE_ENUM_RE = /enum\s+class\s+([A-Za-z_][A-Za-z0-9_]*Divergence)\s*\{([\s\S]*?)\}/g;

function parseEnumMembers(body) {
  return body
    .split(",")
    .map((part) => part.replace(/\/\/.*$/gm, "").replace(/\/\*[\s\S]*?\*\//g, "").trim())
    .filter(Boolean)
    .map((part) => part.split("=")[0].trim()) // drop an explicit initializer, if any
    .filter((name) => /^[A-Za-z_][A-Za-z0-9_]*$/.test(name));
}

function findDeclaredDivergences(root) {
  const enums = []; // { enumName, member, file }
  for (const file of listFiles(SRC_DIR, (f) => /\.(hpp|hh|h|cpp|cc|cxx)$/.test(f))) {
    let content;
    try {
      content = readFileSync(file, "utf8");
    } catch {
      continue;
    }
    if (content.includes("\0")) continue;
    DIVERGENCE_ENUM_RE.lastIndex = 0;
    let m;
    while ((m = DIVERGENCE_ENUM_RE.exec(content))) {
      for (const member of parseEnumMembers(m[2])) {
        enums.push({ enumName: m[1], member, file: file.slice(root.length + 1) });
      }
    }
  }
  return enums;
}

// "Exercised by a fixture" is deliberately textual, not schema-shaped: the
// fixture format (`test/fixtures/schema.json`) has no `divergence:` field of
// its own to add — doing so is a fixture-format change no future L6a issue
// has asked for yet, and Article III.4 rules out pre-building tooling a
// future issue owns. A fixture "exercises" a declared divergence by naming
// it — in its `fixture:` id, a `wrong:` key, a comment, or any other text —
// anywhere in the fixture file itself, or in the sqllogictest that drives
// the row the enum concerns.
function isExercised(member, corpus) {
  return corpus.some((text) => text.includes(member));
}

const declared = findDeclaredDivergences(root);
const corpusFiles = [...listFiles(FIXTURES_DIR, (f) => f.endsWith(".yaml") || f.endsWith(".yml")), ...listFiles(SQL_TEST_DIR, (f) => f.endsWith(".test"))];
const corpus = corpusFiles.map((f) => {
  try {
    return readFileSync(f, "utf8");
  } catch {
    return "";
  }
});

const violations = [];
for (const { enumName, member, file } of declared) {
  if (!isExercised(member, corpus)) {
    violations.push(`${file}: ${enumName}::${member} is a declared divergence no fixture or sqllogictest exercises`);
  }
}

if (violations.length > 0) {
  console.error("divergence-enum-coverage: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log(`divergence-enum-coverage: PASS (${declared.length} declared divergence(s))`);
