#!/usr/bin/env bun
import { $ } from "bun";
import { readFileSync } from "node:fs";

const SCOPE_PREFIXES = ["src/", "test/", "scripts/"];
const DEFERRAL_RE = /\b(later|for now|temporary|will be|not yet|follow-up|TBD)\b/i;

function commentMarkerFor(path) {
  if (/\.(mjs|cpp|cc|cxx|hpp|hh|h|c)$/.test(path)) return "//";
  if (/\.(test|sql|yml|yaml)$/.test(path)) return "#";
  return null;
}

// Deferral language is an excuse to skip something, and belongs in prose —
// comments — not in a string or regex literal (this file's own DEFERRAL_RE
// necessarily contains these words as data, not as a deferral).
export function scanDiffForDeferral(diff) {
  const lines = diff.split("\n");
  let currentFile = null;
  const violations = [];

  for (const line of lines) {
    if (line.startsWith("+++ ")) {
      const path = line.slice(4).trim();
      currentFile = path.startsWith("b/") ? path.slice(2) : path;
      continue;
    }
    if (!line.startsWith("+") || line.startsWith("+++")) continue;
    if (!currentFile) continue;
    if (!SCOPE_PREFIXES.some((p) => currentFile.startsWith(p))) continue;

    const added = line.slice(1);
    const marker = commentMarkerFor(currentFile);
    const markerAt = marker ? added.indexOf(marker) : -1;
    if (markerAt === -1) continue;
    const afterMarker = added.slice(markerAt + marker.length);

    if (DEFERRAL_RE.test(afterMarker) && !/#\d+/.test(added)) {
      violations.push(`${currentFile}: added line uses deferral language without "#<issue>": ${added.trim()}`);
    }
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

  // Net diff, not `--patch` (a per-commit patch series) — see
  // scripts/pr-hygiene.mjs's identical comment (#154).
  const diff =
    diffFileIdx !== -1
      ? readFileSync(args[diffFileIdx + 1], "utf8")
      : await $`gh-tsouza pr diff ${args[prIdx + 1]}`.text();

  const violations = scanDiffForDeferral(diff);
  if (violations.length > 0) {
    console.error("forbid-deferral: FAIL");
    for (const v of violations) console.error(`  ${v}`);
    process.exit(1);
  }
  console.log("forbid-deferral: PASS");
}
