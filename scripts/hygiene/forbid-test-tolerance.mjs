#!/usr/bin/env bun
// The comparator is a single symbol in a single translation unit that no
// test file may shadow (docs/testing/comparator.md's closing paragraph):
// this scans test/ for the word "epsilon" — the term a fixed, ungrounded
// tolerance is named with — and fails if it appears anywhere other than the
// one file that documents the derivation's absence of one,
// test/kernel/comparator_test.cpp. `grep -r epsilon test/` finding only
// that file is this issue's second acceptance criterion; this script is the
// mechanically-enforced version of that same check.
import { $ } from "bun";
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join } from "node:path";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];
const usingRealTree = rootIdx === -1;

const WHITELISTED_PATH = "test/kernel/comparator_test.cpp";
const FORBIDDEN_RE = /\bepsilon\b/i;

async function listFiles(dir) {
  if (usingRealTree) {
    const out = await $`git -C ${dir} ls-files test`.text();
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
  walk(join(dir, "test"), "test");
  return files;
}

const files = await listFiles(root);
const violations = [];

for (const f of files) {
  if (f === WHITELISTED_PATH) continue;
  let content;
  try {
    content = readFileSync(join(root, f), "utf8");
  } catch {
    continue;
  }
  if (content.includes("\0")) continue;
  const lines = content.split("\n");
  for (let i = 0; i < lines.length; i++) {
    if (FORBIDDEN_RE.test(lines[i])) {
      violations.push(`${f}:${i + 1}: forbidden tolerance token "epsilon" outside the comparator (Article V.3)`);
    }
  }
}

if (violations.length > 0) {
  console.error("forbid-test-tolerance: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("forbid-test-tolerance: PASS");
