#!/usr/bin/env bun
// registry-closure — Article V.1: "A function exists only through
// src/kernel/registry.def... A row missing from any roster fails the build,
// named after the row." Two closure directions, both against
// src/kernel/registry.def as the single source of truth:
//
//   1. every registry row has a matching test/sql/<name>.test file (a row
//      without one fails, naming the row) — issue #26's first acceptance
//      criterion.
//   2. no source file under src/ registers a kernel function ad-hoc, outside
//      a registry-driven Register_<name> function — issue #26's third
//      acceptance criterion ("a function in src/functions without a row is
//      red", implemented here against this repo's actual current layout,
//      where both real functions still live directly in
//      src/chronoduck_extension.cpp rather than under a src/functions/ tree).
//
// Parses registry.def with a plain regex over TS_FN(...) lines, matching
// this repo's other hygiene scans' style (a text/regex scan, not a real C
// preprocessor or AST — see forbid-consumer.mjs, workflow-shape.mjs).
import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { join } from "node:path";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const REGISTRY_DEF_PATH = join("src", "kernel", "registry.def");
const REGISTRY_TYPES_PATH = join("src", "kernel", "registry_types.hpp");

// Paths this scan never descends into: the registry files themselves (a row
// name appearing as a macro argument in registry.def, or in registry_types's
// own X-macro-driven static_assert block, is not a "registration call") and
// the vendored submodule trees, which this repo doesn't own.
const EXCLUDED_PREFIXES = ["duckdb/", "extension-ci-tools/"];
const EXCLUDED_PATHS = [REGISTRY_DEF_PATH, REGISTRY_TYPES_PATH];

// name is TS_FN's first argument: a bare identifier, optionally preceded by
// blank space, up to the next comma. Matches this project's convention of
// writing registry.def rows as `TS_FN(name, family, state, det, edge_modes,
// domain, scale_kind)` — see src/kernel/registry.def's header.
const TS_FN_ROW_RE = /^\s*TS_FN\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,/gm;

function listFilesUnder(dir, prefix = "") {
  const files = [];
  let entries;
  try {
    entries = readdirSync(join(dir, prefix));
  } catch {
    return files;
  }
  for (const entry of entries) {
    const rel = prefix ? `${prefix}/${entry}` : entry;
    const full = join(dir, rel);
    const st = statSync(full);
    if (st.isDirectory()) {
      files.push(...listFilesUnder(dir, rel));
    } else {
      files.push(rel);
    }
  }
  return files;
}

function parseRegistryRowNames(root) {
  const defPath = join(root, REGISTRY_DEF_PATH);
  if (!existsSync(defPath)) {
    return null; // caller reports this as its own violation
  }
  const content = readFileSync(defPath, "utf8");
  const names = [];
  let m;
  TS_FN_ROW_RE.lastIndex = 0;
  while ((m = TS_FN_ROW_RE.exec(content))) {
    names.push(m[1]);
  }
  return names;
}

// Check 1 (AC: "A row without a .test file fails the build naming the row").
function checkTestFilePresence(root, names) {
  const violations = [];
  for (const name of names) {
    const testPath = join(root, "test", "sql", `${name}.test`);
    if (!existsSync(testPath)) {
      violations.push(`row "${name}" has no test/sql/${name}.test`);
    }
  }
  return violations;
}

// Finds every `Register_<name>(` function definition's brace extent so check
// 2 can mask them out before looking for ad-hoc registration calls. A
// best-effort brace matcher (skips braces inside string/char literals and
// // and /* */ comments) — sufficient for this codebase's plain K&R-ish
// style, matching this repo's established "regex scan, not a real parser"
// posture for hygiene tooling.
const REGISTER_FN_START_RE = /(?:^|\n)[ \t]*(?:static\s+)?(?:void\s+)?Register_([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{]*\)\s*\{/g;

function findMatchingBrace(content, openBraceIndex) {
  let depth = 0;
  for (let i = openBraceIndex; i < content.length; i++) {
    const c = content[i];
    if (c === "/" && content[i + 1] === "/") {
      const nl = content.indexOf("\n", i);
      i = nl === -1 ? content.length : nl;
      continue;
    }
    if (c === "/" && content[i + 1] === "*") {
      const end = content.indexOf("*/", i + 2);
      i = end === -1 ? content.length : end + 1;
      continue;
    }
    if (c === '"' || c === "'") {
      const quote = c;
      i++;
      while (i < content.length && content[i] !== quote) {
        if (content[i] === "\\") i++;
        i++;
      }
      continue;
    }
    if (c === "{") depth++;
    if (c === "}") {
      depth--;
      if (depth === 0) return i;
    }
  }
  return content.length - 1;
}

// Masks out every Register_<name>(...) { ... } body whose <name> is a real
// registry row, replacing it with spaces (preserving line numbers) so check
// 2's scan for ad-hoc `loader.RegisterFunction(...)`-family calls never fires
// on the registry-driven registration functions themselves. A Register_<foo>
// function whose <foo> is NOT a registry row is deliberately left unmasked —
// registering a kernel function from a function with no matching row is
// exactly the ad-hoc case this check exists to catch.
function maskRegistryDrivenRegistrations(content, validNames) {
  let masked = content;
  REGISTER_FN_START_RE.lastIndex = 0;
  let m;
  const validSet = new Set(validNames);
  const spans = [];
  while ((m = REGISTER_FN_START_RE.exec(content))) {
    const name = m[1];
    if (!validSet.has(name)) continue;
    const openBrace = content.indexOf("{", m.index);
    if (openBrace === -1) continue;
    const closeBrace = findMatchingBrace(content, openBrace);
    spans.push([m.index, closeBrace + 1]);
  }
  for (const [start, end] of spans) {
    const region = masked.slice(start, end);
    const blanked = region.replace(/[^\n]/g, " ");
    masked = masked.slice(0, start) + blanked + masked.slice(end);
  }
  return masked;
}

const AD_HOC_REGISTRATION_RE = /\bloader\s*\.\s*Register(?:Function|AggregateFunction|TableFunction)\s*\(/g;

// Check 2 (AC: "A function in src/functions without a row is red", applied
// to this repo's actual layout — see this file's header comment).
function checkNoAdHocRegistration(root, names) {
  const violations = [];
  const files = listFilesUnder(join(root, "src"), "").map((f) => `src/${f}`);
  for (const f of files) {
    if (!/\.(cpp|cc|cxx|hpp|hh|h)$/.test(f)) continue;
    if (EXCLUDED_PATHS.includes(f)) continue;
    if (EXCLUDED_PREFIXES.some((p) => f.startsWith(`src/${p}`))) continue;
    let content;
    try {
      content = readFileSync(join(root, f), "utf8");
    } catch {
      continue;
    }
    if (content.includes("\0")) continue;

    const masked = maskRegistryDrivenRegistrations(content, names);
    const lines = masked.split("\n");
    for (let i = 0; i < lines.length; i++) {
      AD_HOC_REGISTRATION_RE.lastIndex = 0;
      if (AD_HOC_REGISTRATION_RE.test(lines[i])) {
        violations.push(`${f}:${i + 1}: kernel function registered outside any registry-driven Register_<name> function`);
      }
    }
  }
  return violations;
}

const names = parseRegistryRowNames(root);
if (names === null) {
  console.error("registry-closure: FAIL");
  console.error(`  ${REGISTRY_DEF_PATH} does not exist`);
  process.exit(1);
}

const violations = [...checkTestFilePresence(root, names), ...checkNoAdHocRegistration(root, names)];

if (violations.length > 0) {
  console.error("registry-closure: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("registry-closure: PASS");
