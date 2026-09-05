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

// #47: a citation into build/partners/ — a plain git clone the storage-partner
// harness makes at build time (scripts/partners/rawduck-build.mjs), never a
// submodule, never committed (build/ is gitignored) — is checked STRICTLY
// (existence + exact-one-occurrence, exactly like any other citation) when
// that path exists on disk, but SKIPPED (not a violation) when it doesn't.
// Its absence means only that the partner hasn't been built in this
// environment yet, not that the citation is wrong — a plain `make hygiene`
// run (the fast, submodule-independent merge gate every PR runs) never
// builds the partner, so treating a build/partners/ citation the same as any
// other missing-file citation would fail every PR for a real ADR pointing at
// real source neither this PR nor most PRs will ever check out. The
// `partner-rawduck` lane (docs/testing/lanes.md), which DOES build the
// partner before it runs, calls this same script as one of its own steps —
// that's where these citations get their real, strict check, without
// breaking the merge gate for everyone else. Mirrors this repo's other
// "check needs a thing that isn't always checked out" resolutions: the root
// Makefile's own extension-ci-tools-submodule-absent warning, and
// check-pins.mjs's "pending" (never a failure) report when
// build/partners/rawduck/ hasn't been built yet.
const PARTNER_BUILD_PREFIX = "build/partners/";

// A clang-format/prettier comment reflow can wrap a citation's backtick-fenced
// expression across two `//` lines — per-line-only matching then never sees
// it at all (neither flags it as invalid nor confirms it resolves), silently
// exempting whatever the wrap happens to split (#184: found live on
// `docs/design/surface.md:static-rule:` in src/kernel/registry_types.hpp,
// where the citation's expression was verbatim-correct on one commit and
// silently re-wrapped, still unverified, by the very commit that "fixed" a
// clang-format failure). Mirrors forbid-deferral.mjs's own established
// approach to the same class of bug (its DEFERRAL_RE boundary-join): a
// contiguous run of `//` comment lines is also joined into one string (marker
// and one leading space stripped per line, joined with a single space) and
// scanned the same way, in addition to each line alone — so a citation split
// across a wrap is still caught, on both sides (a real citation that should
// resolve, and a line-number-style citation that should still be forbidden).
const COMMENT_LINE_RE = /^\s*\/\/\s?(.*)$/;

function commentRuns(lines) {
  const runs = [];
  let current = null;
  for (let i = 0; i < lines.length; i++) {
    const m = lines[i].match(COMMENT_LINE_RE);
    if (m) {
      if (!current) {
        current = { startLine: i, parts: [] };
      }
      current.parts.push(m[1]);
    } else if (current) {
      runs.push(current);
      current = null;
    }
  }
  if (current) runs.push(current);
  return runs;
}

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
  const seen = new Set(); // `${line}:${citedFile}:${construct}:${expr}` — a joined-run match at a run's start line dedupes against the identical single-line match for a citation that happened to sit entirely on one line

  function checkCitation(lineNo, citedFile, construct, expr) {
    const key = `${lineNo}:${citedFile}:${construct}:${expr}`;
    if (seen.has(key)) return;
    seen.add(key);
    if (/^\d+$/.test(construct)) {
      violations.push(`${f}:${lineNo}: line-number citation "${citedFile}:${construct}:" is forbidden — cite a construct, never a line number`);
      return;
    }
    const citedPath = join(root, citedFile);
    if (!existsSync(citedPath)) {
      if (citedFile.startsWith(PARTNER_BUILD_PREFIX)) {
        return; // not built in this environment — see PARTNER_BUILD_PREFIX comment above
      }
      violations.push(`${f}:${lineNo}: citation references "${citedFile}", which does not exist`);
      return;
    }
    const citedContent = readFileSync(citedPath, "utf8");
    const occurrences = citedContent.split(expr).length - 1;
    if (occurrences !== 1) {
      violations.push(`${f}:${lineNo}: citation expression \`${expr}\` occurs ${occurrences} times in ${citedFile}, expected exactly 1`);
    }
  }

  for (let i = 0; i < lines.length; i++) {
    let m;
    CITATION_RE.lastIndex = 0;
    while ((m = CITATION_RE.exec(lines[i]))) {
      checkCitation(i + 1, m[1], m[2], m[3]);
    }
  }

  for (const run of commentRuns(lines)) {
    if (run.parts.length < 2) continue; // already covered by the per-line pass above
    const joined = run.parts.join(" ");
    let m;
    CITATION_RE.lastIndex = 0;
    while ((m = CITATION_RE.exec(joined))) {
      checkCitation(run.startLine + 1, m[1], m[2], m[3]);
    }
  }
}

if (violations.length > 0) {
  console.error("verify-citations: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("verify-citations: PASS");
