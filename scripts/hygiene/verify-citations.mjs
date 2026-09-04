#!/usr/bin/env bun
import { $ } from "bun";
import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { join } from "node:path";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];
const usingRealTree = rootIdx === -1;

async function listFiles(dir) {
  if (usingRealTree) {
    const out = await $`git -C ${dir} ls-files`.text();
    return out.split("\n").filter(Boolean);
  }
  const files = [];
  const walk = (d, prefix) => {
    for (const entry of readdirSync(d)) {
      const full = join(d, entry);
      const rel = prefix ? `${prefix}/${entry}` : entry;
      const st = statSync(full);
      if (st.isDirectory()) walk(full, rel);
      else files.push(rel);
    }
  };
  walk(dir, "");
  return files;
}

// A citation is a backtick-fenced "path:construct:" immediately followed by a
// backtick-fenced expression naming the cited construct in that file.
// A line-number-style citation (the construct name is all digits) is always forbidden.
const CITATION_RE = /`([^`\s:]+\.[A-Za-z0-9_]+):([^`:]+):`\s*`([^`]+)`/g;

const files = await listFiles(root);
const violations = [];

for (const f of files) {
  let content;
  try {
    content = readFileSync(join(root, f), "utf8");
  } catch {
    continue;
  }
  if (content.includes("\0")) continue;

  const lines = content.split("\n");
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    let m;
    CITATION_RE.lastIndex = 0;
    while ((m = CITATION_RE.exec(line))) {
      const [, citedFile, construct, expr] = m;
      if (/^\d+$/.test(construct)) {
        violations.push(`${f}:${i + 1}: line-number citation "${citedFile}:${construct}:" is forbidden — cite a construct, never a line number`);
        continue;
      }
      const citedPath = join(root, citedFile);
      if (!existsSync(citedPath)) {
        violations.push(`${f}:${i + 1}: citation references "${citedFile}", which does not exist`);
        continue;
      }
      const citedContent = readFileSync(citedPath, "utf8");
      const occurrences = citedContent.split(expr).length - 1;
      if (occurrences !== 1) {
        violations.push(`${f}:${i + 1}: citation expression \`${expr}\` occurs ${occurrences} times in ${citedFile}, expected exactly 1`);
      }
    }
  }
}

if (violations.length > 0) {
  console.error("verify-citations: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("verify-citations: PASS");
