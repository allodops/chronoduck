#!/usr/bin/env bun
import { readFileSync } from "node:fs";
import { fetchPrDiff } from "../lib/gh-diff.mjs";

const SCOPE_PREFIXES = ["src/", "test/", "scripts/"];
const DEFERRAL_RE = /\b(later|for now|temporary|will be|not yet|follow-up|TBD)\b/i;

// A real "#<issue>" reference: `#` immediately followed by digits, not
// glued to another `#` or word character on its left (so the comment
// marker's own leading digits on a line like "#123 quick and dirty, will
// clean up later" can't masquerade as a citation just because a bare "#\d+"
// regex is unanchored — #166). Tested against the comment text *after* the
// marker is stripped (see afterMarker below), so the marker character
// itself is never a candidate match in the first place.
const ISSUE_REF_RE = /(?:^|[^A-Za-z0-9#])#\d+\b/;

function commentMarkerFor(path) {
  if (/\.(mjs|cpp|cc|cxx|hpp|hh|h|c)$/.test(path)) return "//";
  if (/\.(test|sql|yml|yaml)$/.test(path)) return "#";
  return null;
}

// Deferral language is an excuse to skip something, and belongs in prose —
// comments — not in a string or regex literal (this file's own DEFERRAL_RE
// necessarily contains these words as data, not as a deferral).
//
// Consecutive added lines that are both comments (same file, same comment
// marker, no non-comment line between them) are joined into one logical
// block before either DEFERRAL_RE or ISSUE_REF_RE is tested (#166) — a
// clang-format/prettier wrap can split a two-word deferral phrase ("for
// now", "will be") or its issue-reference exemption across a line break,
// and testing each line independently misses both.
export function scanDiffForDeferral(diff) {
  const lines = diff.split("\n");
  let currentFile = null;
  const violations = [];
  let block = null; // { file, addedLines: string[], afterMarkerParts: string[] }

  function flushBlock() {
    if (!block) return;
    const joinedAfterMarker = block.afterMarkerParts.map((p) => p.trim()).join(" ");
    const joinedAdded = block.addedLines.map((l) => l.trim()).join(" ");
    if (DEFERRAL_RE.test(joinedAfterMarker) && !ISSUE_REF_RE.test(joinedAfterMarker)) {
      violations.push(`${block.file}: added line(s) use deferral language without "#<issue>": ${joinedAdded}`);
    }
    block = null;
  }

  for (const line of lines) {
    if (line.startsWith("+++ ")) {
      flushBlock();
      const path = line.slice(4).trim();
      currentFile = path.startsWith("b/") ? path.slice(2) : path;
      continue;
    }
    if (!line.startsWith("+") || line.startsWith("+++")) {
      flushBlock();
      continue;
    }
    if (!currentFile || !SCOPE_PREFIXES.some((p) => currentFile.startsWith(p))) {
      flushBlock();
      continue;
    }

    const added = line.slice(1);
    const marker = commentMarkerFor(currentFile);
    const markerAt = marker ? added.indexOf(marker) : -1;
    if (markerAt === -1) {
      flushBlock();
      continue;
    }
    const afterMarker = added.slice(markerAt + marker.length);

    if (block && block.file === currentFile) {
      block.addedLines.push(added);
      block.afterMarkerParts.push(afterMarker);
    } else {
      flushBlock();
      block = { file: currentFile, addedLines: [added], afterMarkerParts: [afterMarker] };
    }
  }
  flushBlock();
  return violations;
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  const prIdx = args.indexOf("--pr");
  const diffFileIdx = args.indexOf("--diff-file");

  if (prIdx === -1 && diffFileIdx === -1) {
    console.error("forbid-deferral: usage: forbid-deferral.mjs (--pr <n> | --diff-file <path>)");
    process.exit(2);
  }

  const diff = diffFileIdx !== -1 ? readFileSync(args[diffFileIdx + 1], "utf8") : await fetchPrDiff(args[prIdx + 1]);

  const violations = scanDiffForDeferral(diff);
  if (violations.length > 0) {
    console.error("forbid-deferral: FAIL");
    for (const v of violations) console.error(`  ${v}`);
    process.exit(1);
  }
  console.log("forbid-deferral: PASS");
}
