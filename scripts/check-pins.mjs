#!/usr/bin/env bun
// Pin-consistency check (Article IV): the duckdb and extension-ci-tools submodule
// pins must agree with each other on the versions T0.3 fixes, and — once
// .github/workflows/MainDistributionPipeline.yml exists (T0.4) — with the
// duckdb_version/ci_tools_version it declares. Absent that file, the workflow
// side is reported "pending", never a failure: this task doesn't own that file.
import { $ } from "bun";
import { readFileSync, existsSync } from "node:fs";
import { join } from "node:path";
import { parse } from "yaml";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

// The exact pins T0.3 fixes (CONSTITUTION.md Article IV / issue #16): duckdb at
// the tag v1.5.4, extension-ci-tools at the branch v1.5-variegata.
const EXPECTED = {
  duckdb: "v1.5.4",
  "extension-ci-tools": "v1.5-variegata",
};

async function submodulePin(name) {
  let out;
  try {
    out = await $`git -C ${root} submodule status -- ${name}`.text();
  } catch (e) {
    return { sha: null, ref: null, error: `could not read submodule status: ${e.message}` };
  }
  const line = out.trim();
  if (!line) return { sha: null, ref: null, error: "not a registered submodule" };
  // Format: "[+-U]<sha> <path> (<describe>)" — the describe suffix is what we
  // compare against. A branch checkout describes as "heads/<branch>" when a
  // local branch exists, or "remotes/<remote>/<branch>" on a fresh
  // `submodule update --init` (detached HEAD, no local branch — only the
  // remote-tracking ref) — both name the same branch, so strip either.
  // A tag checkout describes as the bare tag name, needing no stripping.
  const m = line.match(/^[ +\-U]?([0-9a-f]{40})\s+\S+(?:\s+\(([^)]+)\))?/);
  if (!m) return { sha: null, ref: null, error: `unparsable submodule status line: "${line}"` };
  const [, sha, describe] = m;
  const ref = describe ? describe.replace(/^heads\//, "").replace(/^remotes\/[^/]+\//, "") : null;
  return { sha, ref, error: null };
}

async function workflowVersions() {
  const wfPath = join(root, ".github", "workflows", "MainDistributionPipeline.yml");
  if (!existsSync(wfPath)) return { present: false, duckdb: null, ciTools: null, error: null };

  let doc;
  try {
    doc = parse(readFileSync(wfPath, "utf8"));
  } catch (e) {
    return { present: true, duckdb: null, ciTools: null, error: `could not parse YAML (${e.message})` };
  }

  const jobs = Object.values(doc?.jobs ?? {});
  const duckdbVersions = new Set();
  const ciToolsVersions = new Set();
  for (const job of jobs) {
    const withArgs = job?.with ?? {};
    if (withArgs.duckdb_version) duckdbVersions.add(String(withArgs.duckdb_version));
    if (withArgs.ci_tools_version) ciToolsVersions.add(String(withArgs.ci_tools_version));
  }

  if (duckdbVersions.size > 1) {
    return { present: true, duckdb: null, ciTools: null, error: `jobs disagree on duckdb_version: ${[...duckdbVersions].join(", ")}` };
  }
  if (ciToolsVersions.size > 1) {
    return { present: true, duckdb: null, ciTools: null, error: `jobs disagree on ci_tools_version: ${[...ciToolsVersions].join(", ")}` };
  }

  return {
    present: true,
    duckdb: duckdbVersions.size ? [...duckdbVersions][0] : null,
    ciTools: ciToolsVersions.size ? [...ciToolsVersions][0] : null,
    error: null,
  };
}

const violations = [];
const notes = [];

const duckdbPin = await submodulePin("duckdb");
const ciToolsPin = await submodulePin("extension-ci-tools");

if (duckdbPin.error) {
  violations.push(`duckdb submodule: ${duckdbPin.error}`);
} else if (duckdbPin.ref !== EXPECTED.duckdb) {
  violations.push(`duckdb submodule pinned at "${duckdbPin.ref ?? duckdbPin.sha}", expected "${EXPECTED.duckdb}"`);
} else {
  notes.push(`duckdb submodule pin: ${duckdbPin.ref} (${duckdbPin.sha}) — OK`);
}

if (ciToolsPin.error) {
  violations.push(`extension-ci-tools submodule: ${ciToolsPin.error}`);
} else if (ciToolsPin.ref !== EXPECTED["extension-ci-tools"]) {
  violations.push(
    `extension-ci-tools submodule pinned at "${ciToolsPin.ref ?? ciToolsPin.sha}", expected "${EXPECTED["extension-ci-tools"]}"`
  );
} else {
  notes.push(`extension-ci-tools submodule pin: ${ciToolsPin.ref} (${ciToolsPin.sha}) — OK`);
}

const wf = await workflowVersions();
if (wf.error) {
  violations.push(`.github/workflows/MainDistributionPipeline.yml: ${wf.error}`);
} else if (!wf.present) {
  notes.push("duckdb_version (workflow): pending — .github/workflows/MainDistributionPipeline.yml does not exist yet (T0.4)");
  notes.push("ci_tools_version (workflow): pending — .github/workflows/MainDistributionPipeline.yml does not exist yet (T0.4)");
} else {
  if (wf.duckdb !== EXPECTED.duckdb) {
    violations.push(`workflow duckdb_version "${wf.duckdb}" does not match "${EXPECTED.duckdb}"`);
  } else {
    notes.push(`workflow duckdb_version: ${wf.duckdb} — OK`);
  }
  if (wf.ciTools !== EXPECTED["extension-ci-tools"]) {
    violations.push(`workflow ci_tools_version "${wf.ciTools}" does not match "${EXPECTED["extension-ci-tools"]}"`);
  } else {
    notes.push(`workflow ci_tools_version: ${wf.ciTools} — OK`);
  }
}

for (const n of notes) console.log(n);

if (violations.length > 0) {
  console.error("check-pins: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("check-pins: PASS");
