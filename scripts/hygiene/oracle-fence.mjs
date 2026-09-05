#!/usr/bin/env bun
// oracle-fence — T5 (docs/testing/rules.md): "Oracles never import the
// kernel. The from-scratch evaluator and the fixture harness live in a
// separate build target whose include path cannot reach the extension's
// sources; a meta-test walks includes and fails on any edge." AGENTS.md's
// own "Where things are" table states the same rule for this repo:
// "test/oracle/ — the from-scratch oracle (must never include `src/`)."
//
// Walks every `#include "..."` (quoted, relative — never `#include <...>`,
// which is always a standard/system header and never a path in this
// repository) reachable from every file under `test/oracle/`, transitively,
// resolving each include relative to the including file's own directory
// (matching how a C++ compiler itself resolves a quoted include), and fails
// if any resolved path lands under `src/`. This is the actual "walk includes
// and fails on any edge" meta-test T5 asks for — a real graph walk, not a
// single-directory grep, so a `test/oracle/a.hpp` that includes a
// `test/oracle/b.hpp` that includes `src/kernel/comparator.hpp` is still
// caught even though `a.hpp` itself never spells `src/` anywhere.
import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { join, dirname, normalize, relative, sep } from "node:path";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const ORACLE_DIR = join(root, "test", "oracle");
const INCLUDE_RE = /^\s*#\s*include\s*"([^"]+)"/gm;
const CPP_EXT_RE = /\.(hpp|hh|h|hxx|cpp|cc|cxx)$/;

function listFiles(dir) {
  if (!existsSync(dir)) return [];
  const out = [];
  const walk = (d) => {
    for (const entry of readdirSync(d).sort()) {
      const full = join(d, entry);
      const st = statSync(full);
      if (st.isDirectory()) walk(full);
      else if (CPP_EXT_RE.test(entry)) out.push(full);
    }
  };
  walk(dir);
  return out;
}

function quotedIncludes(path) {
  const content = readFileSync(path, "utf8");
  const out = [];
  let m;
  INCLUDE_RE.lastIndex = 0;
  while ((m = INCLUDE_RE.exec(content))) out.push(m[1]);
  return out;
}

function toPosixRelative(fromRoot) {
  return relative(root, fromRoot).split(sep).join("/");
}

const oracleFiles = listFiles(ORACLE_DIR);
if (oracleFiles.length === 0) {
  console.error("oracle-fence: FAIL");
  console.error(`  test/oracle has no .hpp/.cpp files to fence (Article V.2 / AGENTS.md: the from-scratch oracle lives there)`);
  process.exit(1);
}

const violations = [];
const visited = new Set();

function walk(absPath, chain) {
  const key = normalize(absPath);
  if (visited.has(key)) return;
  visited.add(key);
  if (!existsSync(absPath)) return; // an include this scan can't resolve — nothing further to walk

  for (const inc of quotedIncludes(absPath)) {
    const resolved = normalize(join(dirname(absPath), inc));
    const relPath = toPosixRelative(resolved);
    const nextChain = [...chain, relPath];
    if (relPath === "src" || relPath.startsWith("src/")) {
      violations.push(`${chain[0]}: reaches ${relPath} via ${nextChain.join(" -> ")}`);
      continue; // don't also walk into src/ — the violation is already recorded
    }
    walk(resolved, nextChain);
  }
}

for (const file of oracleFiles) {
  const rel = toPosixRelative(file);
  walk(file, [rel]);
}

if (violations.length > 0) {
  console.error("oracle-fence: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log(`oracle-fence: PASS (${oracleFiles.length} file(s) under test/oracle, no edge reaches src/)`);
