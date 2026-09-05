#!/usr/bin/env bun
// #229: test/kernel/*.cpp used to reach its own kernel headers via a
// fragile parent-relative include (`#include "../../src/kernel/foo.hpp"`) —
// the only reason that ever compiled is that kernel-primitive-tests.mjs's
// g++ invocation passed no -I flag at all, so a header could only resolve
// relative to the including file itself. Now that invocation passes
// `-I <root>/src`, every test/kernel/*.cpp reaches its own kernel headers
// with a clean, bare quoted include (`#include "kernel/foo.hpp"`) — the same
// form `src/chronoduck_extension.cpp` already used before #229 ever touched
// this tree, since src/ (not the repo root) is this codebase's established
// header root. `-I <root>` stays alongside it purely for
// `oracle_sweep_test.cpp`'s `#include "test/oracle/foo.hpp"`, a
// cross-top-level-directory reference with no `src/` precedent to follow.
// Either way, a parent-relative include anywhere under test/kernel/ is
// always a regression back to the fragile pre-#229 form. This scan fails
// the moment one reappears, naming the offending file and line — the
// mechanical, permanent half of #229's fix, mirroring
// forbid-test-tolerance.mjs's shape for the same directory.
import { $ } from "bun";
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join } from "node:path";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];
const usingRealTree = rootIdx === -1;

const KERNEL_TEST_DIR = "test/kernel";
const FORBIDDEN_RE = /#include\s*"\.\.\//;

async function listFiles(dir) {
  if (usingRealTree) {
    const out = await $`git -C ${dir} ls-files ${KERNEL_TEST_DIR}`.text();
    return out.split("\n").filter(Boolean);
  }
  const files = [];
  const walk = (d, prefix) => {
    let entries;
    try {
      entries = readdirSync(d);
    } catch {
      return;
    }
    for (const entry of entries) {
      const full = join(d, entry);
      const rel = prefix ? `${prefix}/${entry}` : entry;
      const st = statSync(full);
      if (st.isDirectory()) walk(full, rel);
      else files.push(rel);
    }
  };
  walk(join(dir, KERNEL_TEST_DIR), KERNEL_TEST_DIR);
  return files;
}

const files = await listFiles(root);
const violations = [];

for (const f of files) {
  let content;
  try {
    content = readFileSync(join(root, f), "utf8");
  } catch {
    continue; // unreadable or removed mid-scan, skip
  }
  if (content.includes("\0")) continue; // binary
  const lines = content.split("\n");
  for (let i = 0; i < lines.length; i++) {
    if (FORBIDDEN_RE.test(lines[i])) {
      violations.push(`${f}:${i + 1}: parent-relative include reintroduces the fragile pre-#229 form`);
    }
  }
}

if (violations.length > 0) {
  console.error("forbid-relative-kernel-include: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("forbid-relative-kernel-include: PASS");
