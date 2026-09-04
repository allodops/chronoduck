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

function tailWords(text, n) {
  const words = text.trim().split(/\s+/).filter(Boolean);
  return words.slice(-n).join(" ");
}

function headWords(text, n) {
  const words = text.trim().split(/\s+/).filter(Boolean);
  return words.slice(0, n).join(" ");
}

// Deferral language is an excuse to skip something, and belongs in prose —
// comments — not in a string or regex literal (this file's own DEFERRAL_RE
// necessarily contains these words as data, not as a deferral).
//
// Every added comment line is tested on its own first — unchanged from
// before #166. On top of that, a *narrow boundary snippet* (the last few
// words of the previous added comment line plus the first few words of
// this one, same file, immediately consecutive) is tested for a phrase
// that straddles the line break and doesn't appear whole on either line
// alone — a clang-format/prettier wrap can split a two-word phrase ("for
// now", "will be", "not yet") across two lines, and per-line-only testing
// misses that (#166). The exemption for a spanning violation is checked
// only against the *later* of the two lines, never the earlier one: an
// earlier version tested the exemption against the whole joined block
// (or, in a since-abandoned narrower fix, a boundary snippet that could
// still absorb a short earlier line's content wholesale), either of which
// let a real "#<issue>" reference on one line exempt a wholly unrelated
// deferral violation on the very next, adjacent — but semantically
// separate — comment line (#178). A wrapped sentence's own trailing
// citation naturally lands on the line where the sentence ends, so
// requiring the reference there (not on whichever earlier line happens to
// be adjacent) matches how these comments are actually written, and can't
// be satisfied by an unrelated prior comment no matter how short it is.
export function scanDiffForDeferral(diff) {
  const lines = diff.split("\n");
  let currentFile = null;
  const violations = [];
  let prev = null; // { added, afterMarker } of the previous added comment line, if immediately preceding

  for (const line of lines) {
    if (line.startsWith("+++ ")) {
      const path = line.slice(4).trim();
      currentFile = path.startsWith("b/") ? path.slice(2) : path;
      prev = null;
      continue;
    }
    if (!line.startsWith("+") || line.startsWith("+++")) {
      prev = null;
      continue;
    }
    if (!currentFile || !SCOPE_PREFIXES.some((p) => currentFile.startsWith(p))) {
      prev = null;
      continue;
    }

    const added = line.slice(1);
    const marker = commentMarkerFor(currentFile);
    const markerAt = marker ? added.indexOf(marker) : -1;
    if (markerAt === -1) {
      prev = null;
      continue;
    }
    const afterMarker = added.slice(markerAt + marker.length);

    let flagged = false;
    if (DEFERRAL_RE.test(afterMarker) && !ISSUE_REF_RE.test(afterMarker)) {
      violations.push(`${currentFile}: added line(s) use deferral language without "#<issue>": ${added.trim()}`);
      flagged = true;
    } else if (
      !flagged &&
      prev !== null &&
      !DEFERRAL_RE.test(prev.afterMarker) &&
      !DEFERRAL_RE.test(afterMarker) &&
      DEFERRAL_RE.test(`${tailWords(prev.afterMarker, 4)} ${headWords(afterMarker, 4)}`) &&
      !ISSUE_REF_RE.test(afterMarker)
    ) {
      violations.push(`${currentFile}: added line(s) use deferral language without "#<issue>": ${prev.added.trim()} ${added.trim()}`);
    }

    prev = { added, afterMarker };
  }
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
