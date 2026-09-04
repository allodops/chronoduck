#!/usr/bin/env bun
import { $ } from "bun";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];
const baseIdx = args.indexOf("--base");
let base = baseIdx === -1 ? null : args[baseIdx + 1];

async function tryResolve(candidate) {
  try {
    await $`git -C ${root} rev-parse --verify ${candidate}`.quiet();
    return true;
  } catch {
    return false;
  }
}

async function resolveBase() {
  if (base) return base;
  for (const candidate of ["origin/main", "main"]) {
    if (await tryResolve(candidate)) return candidate;
  }
  // A PR-triggered CI checkout is typically shallow and only fetches the ref
  // needed for the merge commit — origin/main may genuinely not be a
  // resolvable local ref yet, not because there's no base to diff against.
  // Fetch it explicitly before concluding there's nothing to compare.
  try {
    await $`git -C ${root} fetch origin main:refs/remotes/origin/main`.quiet();
  } catch {
    // network/remote unavailable — fall through to "no base" below
  }
  if (await tryResolve("origin/main")) return "origin/main";
  return null;
}

function versionOf(text) {
  const m = text.match(/\*\*Version\*\*\s+([\d.]+)/);
  return m ? m[1] : null;
}

function lastAmendedOf(text) {
  const m = text.match(/\*\*Last amended\*\*\s+([^\n*]+)/);
  return m ? m[1].trim() : null;
}

function versionGreater(a, b) {
  const pa = a.split(".").map(Number);
  const pb = b.split(".").map(Number);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const x = pa[i] ?? 0, y = pb[i] ?? 0;
    if (x !== y) return x > y;
  }
  return false;
}

base = await resolveBase();
if (!base) {
  // Consistent with Article II.3's fail-closed stance: unable to verify means
  // red, never a silent pass — this check exists precisely to catch an
  // unamended constitution change, and a missing base ref can't rule that out.
  console.error("constitution-check: FAIL (no base ref to diff against — cannot verify CONSTITUTION.md)");
  process.exit(1);
}

// Two-dot, not three: compares file *content* between the two tips directly,
// which needs no shared history — a shallow CI checkout's base and HEAD often
// have no local merge-base even though both refs resolve fine individually.
const changedFiles = (await $`git -C ${root} diff --name-only ${base} HEAD`.text()).split("\n").filter(Boolean);

if (!changedFiles.includes("CONSTITUTION.md")) {
  console.log("constitution-check: PASS (CONSTITUTION.md unchanged)");
  process.exit(0);
}

const violations = [];

let oldText = "";
try {
  oldText = await $`git -C ${root} show ${base}:CONSTITUTION.md`.text();
} catch {
  oldText = ""; // CONSTITUTION.md is new in this diff
}
let newText = "";
try {
  newText = await $`git -C ${root} show HEAD:CONSTITUTION.md`.text();
} catch {
  newText = await Bun.file(`${root}/CONSTITUTION.md`).text();
}

const oldVersion = versionOf(oldText);
const newVersion = versionOf(newText);
if (!newVersion || (oldVersion && !versionGreater(newVersion, oldVersion))) {
  violations.push(`CONSTITUTION.md changed but Version did not increase (${oldVersion ?? "none"} -> ${newVersion ?? "none"})`);
}

const oldAmended = lastAmendedOf(oldText);
const newAmended = lastAmendedOf(newText);
if (!newAmended || newAmended === oldAmended) {
  violations.push(`CONSTITUTION.md changed but Last amended date was not updated`);
}

const addedAdrs = (
  await $`git -C ${root} diff --name-status ${base} HEAD -- docs/decisions/`.text()
)
  .split("\n")
  .filter((l) => l.startsWith("A\t"))
  .map((l) => l.slice(2));

let hasAcceptedAdr = false;
for (const adr of addedAdrs) {
  const text = await $`git -C ${root} show HEAD:${adr}`.text();
  if (/status:\s*accepted/.test(text)) {
    hasAcceptedAdr = true;
    break;
  }
}
if (!hasAcceptedAdr) {
  violations.push("CONSTITUTION.md changed but no new docs/decisions/*.md with `status: accepted` was added");
}

if (violations.length > 0) {
  console.error("constitution-check: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("constitution-check: PASS");
