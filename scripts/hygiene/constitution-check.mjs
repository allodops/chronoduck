#!/usr/bin/env bun
import { $ } from "bun";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];
const baseIdx = args.indexOf("--base");
let base = baseIdx === -1 ? null : args[baseIdx + 1];

async function resolveBase() {
  if (base) return base;
  for (const candidate of ["origin/main", "main"]) {
    try {
      await $`git -C ${root} rev-parse --verify ${candidate}`.quiet();
      return candidate;
    } catch {
      // try next
    }
  }
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
  console.log("constitution-check: PASS (no base ref to diff against)");
  process.exit(0);
}

const changedFiles = (await $`git -C ${root} diff --name-only ${base}...HEAD`.text()).split("\n").filter(Boolean);

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
  await $`git -C ${root} diff --name-status ${base}...HEAD -- docs/decisions/`.text()
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
