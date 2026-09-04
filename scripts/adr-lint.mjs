#!/usr/bin/env bun
// make adr-lint
// Enforces docs/decisions/*.md's shape (Article IX.1/IX.2): filename matches
// \d{4}-[a-z0-9-]+.md, numbers form one contiguous sequence from 0000 (the
// template), front matter `status` is one of proposed/accepted/deprecated/
// superseded with an ISO date, and a superseded ADR names its successor.
// 0000-template.md is exempt from the front-matter content checks (its
// values are placeholders, not a real decision record) but still occupies
// its slot in the numbering sequence.
import { readFileSync, readdirSync, existsSync } from "node:fs";
import { join } from "node:path";
import { parse } from "yaml";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const FILENAME_RE = /^(\d{4})-([a-z0-9-]+)\.md$/;
const STATUSES = ["proposed", "accepted", "deprecated", "superseded"];
const ISO_DATE_RE = /^\d{4}-\d{2}-\d{2}$/;

const dir = join(root, "docs", "decisions");
const violations = [];

if (!existsSync(dir)) {
  console.error("adr-lint: FAIL");
  console.error("  docs/decisions/ does not exist");
  process.exit(1);
}

const files = readdirSync(dir).filter((f) => f.endsWith(".md"));
const numbered = [];

for (const file of files) {
  const m = file.match(FILENAME_RE);
  if (!m) {
    violations.push(`${file}: filename does not match \\d{4}-[a-z0-9-]+.md`);
    continue;
  }
  numbered.push({ file, number: Number(m[1]) });
}

const numbers = numbered.map((n) => n.number).sort((a, b) => a - b);
for (let i = 0; i < numbers.length; i++) {
  if (numbers[i] !== i) {
    violations.push(`numbering gap or duplicate: expected ${String(i).padStart(4, "0")}, found ${numbers[i] !== undefined ? String(numbers[i]).padStart(4, "0") : "nothing"}`);
    break;
  }
}

for (const { file } of numbered) {
  if (file === "0000-template.md") continue;

  const content = readFileSync(join(dir, file), "utf8");
  const fmMatch = content.match(/^---\n([\s\S]*?)\n---/);
  if (!fmMatch) {
    violations.push(`${file}: missing front matter`);
    continue;
  }

  let fm;
  try {
    fm = parse(fmMatch[1]) ?? {};
  } catch (e) {
    violations.push(`${file}: front matter is not valid YAML (${e.message})`);
    continue;
  }

  if (!STATUSES.includes(fm.status)) {
    violations.push(`${file}: status "${fm.status}" is not one of ${STATUSES.join(", ")}`);
  }
  if (typeof fm.date !== "string" || !ISO_DATE_RE.test(fm.date)) {
    violations.push(`${file}: date "${fm.date}" is not an ISO date (YYYY-MM-DD)`);
  }
  if (fm.status === "superseded" && !fm.superseded_by) {
    violations.push(`${file}: status is "superseded" but front matter has no "superseded_by" field naming its successor`);
  }
}

if (violations.length > 0) {
  console.error("adr-lint: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log(`adr-lint: PASS (${numbered.length} ADR(s), numbered 0000-${String(numbered.length - 1).padStart(4, "0")})`);
