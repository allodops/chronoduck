#!/usr/bin/env bun
// Pin-consistency check (Article IV): the duckdb and extension-ci-tools submodule
// pins must agree with each other on the versions T0.3 fixes, and — once
// .github/workflows/MainDistributionPipeline.yml exists (T0.4) — with the
// duckdb_version/ci_tools_version it declares. Absent that file, the workflow
// side is reported "pending", never a failure: this task doesn't own that file.
//
// Also reports (#47) the storage-partner pin declared in
// scripts/partners/rawduck.json alongside the submodule pins above, and fails
// when that pinned commit disagrees with what's actually checked out at
// build/partners/rawduck/ — but only when that directory exists. It's a
// plain `git clone` (never a submodule, never committed — see
// scripts/partners/rawduck-build.mjs), so a hygiene-only run that never built
// the partner has nothing to compare against; that absence is reported
// "pending", exactly like the MainDistributionPipeline.yml case above and
// the Makefile's own "extension-ci-tools submodule not checked out" warning
// (never a failure — the check that does build the partner first, the
// partner-rawduck lane, is where this comparison actually bites).
//
// Same shape again (#43) for the libchdb pin declared in
// scripts/live-oracles/chdb.json: also never a submodule, never committed —
// scripts/live-oracles/chdb-fetch.mjs fetches a checksummed release tarball
// into build/live-oracles/chdb/<tag>/ instead of cloning a source tree. There
// is no independent HEAD to compare against (there's no git checkout at
// all); chdb-fetch.mjs itself already refuses to vendor anything whose
// sha256 doesn't match the pin, so the fact that libchdb.so and chdb.h exist
// at the pinned tag's own directory is the "actual matches pinned" evidence
// here. Absent that directory, it's "pending" (run `make chdb-fetch`), same
// as the rawduck case never being a failure on its own.
import { $ } from "bun";
import { readFileSync, existsSync } from "node:fs";
import { join } from "node:path";
import { parse } from "yaml";
import { EXPECTED_DUCKDB_REF } from "./lib/duckdb-pin.mjs";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

// The exact pins T0.3 fixes (CONSTITUTION.md Article IV / issue #16): duckdb at
// the tag v1.5.4, extension-ci-tools at the branch v1.5-variegata.
const EXPECTED = {
  duckdb: EXPECTED_DUCKDB_REF,
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

// Partner pin (#47): scripts/partners/rawduck.json names the commit
// build/partners/rawduck/ (a plain clone, never a submodule) must actually be
// checked out at, once that clone exists.
async function partnerPinStatus(rootDir) {
  const configPath = join(rootDir, "scripts", "partners", "rawduck.json");
  if (!existsSync(configPath)) {
    return { present: false };
  }
  let config;
  try {
    config = JSON.parse(readFileSync(configPath, "utf8"));
  } catch (e) {
    return { present: true, error: `could not parse ${configPath} (${e.message})` };
  }
  const checkoutPath = join(rootDir, "build", "partners", "rawduck");
  if (!existsSync(checkoutPath)) {
    return { present: true, pinned: config.commit, checkedOut: false };
  }
  let actual;
  try {
    actual = (await $`git -C ${checkoutPath} rev-parse HEAD`.text()).trim();
  } catch (e) {
    return { present: true, pinned: config.commit, checkedOut: true, error: `could not read HEAD of ${checkoutPath}: ${e.message}` };
  }
  return { present: true, pinned: config.commit, checkedOut: true, actual };
}

const partnerPin = await partnerPinStatus(root);
if (partnerPin.present) {
  if (partnerPin.error) {
    violations.push(`rawduck partner pin: ${partnerPin.error}`);
  } else if (!partnerPin.checkedOut) {
    notes.push(`rawduck partner pin: ${partnerPin.pinned} — pending (build/partners/rawduck/ not checked out; run \`make partner-rawduck-build\`)`);
  } else if (partnerPin.actual !== partnerPin.pinned) {
    violations.push(
      `rawduck partner pin "${partnerPin.pinned}" (scripts/partners/rawduck.json) does not match the checkout at build/partners/rawduck/, which is at "${partnerPin.actual}"`
    );
  } else {
    notes.push(`rawduck partner pin: ${partnerPin.pinned} — OK (matches build/partners/rawduck/ checkout)`);
  }
}

// libchdb pin (#43): scripts/live-oracles/chdb.json names the pinned
// (repository, tag, per-platform sha256) scripts/live-oracles/chdb-fetch.mjs
// vendors into build/live-oracles/chdb/<tag>/ (never a submodule, never
// committed — see that script's own header comment).
async function libchdbPinStatus(rootDir) {
  const configPath = join(rootDir, "scripts", "live-oracles", "chdb.json");
  if (!existsSync(configPath)) return { present: false };
  let config;
  try {
    config = JSON.parse(readFileSync(configPath, "utf8"));
  } catch (e) {
    return { present: true, error: `could not parse ${configPath} (${e.message})` };
  }
  if (!config.repository || !config.tag) {
    return { present: true, error: `${configPath} must declare "repository" and "tag"` };
  }
  const pinned = `${config.repository}@${config.tag}`;
  // Only linux-x86_64 is pinned today (chdb.json's own comment: the only
  // platform the chdb-differential CI lane runs on) — the same
  // "no pin for this platform" case chdb-fetch.mjs itself fails loudly on,
  // reported here as a violation for the same reason, not silently skipped.
  const platformKey = process.platform === "linux" && process.arch === "x64" ? "linux-x86_64" : null;
  const platformPin = platformKey ? config.platforms?.[platformKey] : null;
  if (!platformKey || !platformPin) {
    return { present: true, pinned, error: `no pinned libchdb asset for this platform (${process.platform}/${process.arch}) in ${configPath}` };
  }
  const vendoredRel = join("build", "live-oracles", "chdb", config.tag);
  const vendoredDir = join(rootDir, vendoredRel);
  const vendored = existsSync(join(vendoredDir, "libchdb.so")) && existsSync(join(vendoredDir, "chdb.h"));
  return { present: true, pinned, sha256: platformPin.sha256, vendoredRel, vendored, error: null };
}

const libchdbPin = await libchdbPinStatus(root);
if (libchdbPin.present) {
  if (libchdbPin.error) {
    violations.push(`libchdb pin: ${libchdbPin.error}`);
  } else if (!libchdbPin.vendored) {
    notes.push(`libchdb pin: ${libchdbPin.pinned} — pending (${libchdbPin.vendoredRel} not vendored; run \`make chdb-fetch\`)`);
  } else {
    notes.push(`libchdb pin: ${libchdbPin.pinned} — OK (vendored at ${libchdbPin.vendoredRel}, sha256 ${libchdbPin.sha256} verified on fetch)`);
  }
}

for (const n of notes) console.log(n);

if (violations.length > 0) {
  console.error("check-pins: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("check-pins: PASS");
