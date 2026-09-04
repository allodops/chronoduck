#!/usr/bin/env bun
// make docs-links
// Resolves every relative markdown link and #anchor under docs/ and
// README.md: a relative path must exist, and an #anchor must match some
// heading's generated slug in the target file (GitHub's algorithm —
// lowercase, spaces to hyphens, strip anything but letters/digits/hyphens/
// underscores). An http(s):// link is never checked (no network access).
import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { dirname, join, relative } from "node:path";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const LINK_RE = /\[[^\]]*\]\(([^)]+)\)/g;

export function slugify(heading) {
  return heading
    .trim()
    .toLowerCase()
    .replace(/\s+/g, "-")
    .replace(/[^\w-]/g, "");
}

export function headingSlugs(markdown) {
  const slugs = new Set();
  for (const line of markdown.split("\n")) {
    const m = line.match(/^#{1,6}\s+(.+)$/);
    if (m) slugs.add(slugify(m[1]));
  }
  return slugs;
}

function listMarkdownFiles(dir, out = []) {
  for (const entry of readdirSync(dir)) {
    const full = join(dir, entry);
    const st = statSync(full);
    if (st.isDirectory()) listMarkdownFiles(full, out);
    else if (entry.endsWith(".md")) out.push(full);
  }
  return out;
}

// Guarded so importing this module for its pure functions (hygiene-selftest)
// never scans the filesystem or calls process.exit().
if (import.meta.main) {
  const files = [];
  const docsDir = join(root, "docs");
  if (existsSync(docsDir)) listMarkdownFiles(docsDir, files);
  const readmePath = join(root, "README.md");
  if (existsSync(readmePath)) files.push(readmePath);

  const violations = [];

  for (const file of files) {
    const content = readFileSync(file, "utf8");
    const rel = relative(root, file);
    let m;
    LINK_RE.lastIndex = 0;
    while ((m = LINK_RE.exec(content))) {
      const target = m[1];
      if (/^https?:\/\//.test(target)) continue;

      const [pathPart, anchor] = target.split("#");
      let targetFile = file;
      if (pathPart) {
        targetFile = join(dirname(file), pathPart);
        if (!existsSync(targetFile)) {
          violations.push(`${rel}: dead link to "${pathPart}"`);
          continue;
        }
      }
      if (anchor) {
        const targetContent = targetFile === file ? content : readFileSync(targetFile, "utf8");
        if (!headingSlugs(targetContent).has(anchor)) {
          violations.push(`${rel}: dead anchor "#${anchor}" in "${pathPart || "(same file)"}"`);
        }
      }
    }
  }

  if (violations.length > 0) {
    console.error("docs-links: FAIL");
    for (const v of violations) console.error(`  ${v}`);
    process.exit(1);
  }
  console.log(`docs-links: PASS (${files.length} file(s) scanned)`);
}
