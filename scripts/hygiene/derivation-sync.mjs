#!/usr/bin/env bun
// make derivation-sync
// #37's own sync test: "test/fixtures/derived/ receives files from the
// derivation repository; a sync test compares the derivation manifest
// against the roster and flags UNRECORDED; the tool's version is in every
// provenance" (issue #37's Goal) — the same UNRECORDED vocabulary
// docs/testing/corpora.md's own promqltest row already names for this exact
// drop-in pipeline: "A derivation tool parses the scripts, isolates
// single-function range evals, and emits neutral fixtures with provenance
// prometheus-<version>; a sync test on the kernel side treats new fixtures
// as UNRECORDED."
//
// The comparison is against `test/fixtures/derived/manifest.json` — the
// derivation tool's own declared inventory of what it emitted this batch —
// never against the tree's actual file list alone, because a plain file
// count can't catch the failure mode corpora.md's own closing line names:
// "A derivation tool that silently drops evals it cannot parse would shrink
// the kernel's corpus without anything going red." A manifest that always
// agreed with whatever files happen to be present could never catch that;
// this script treats the manifest as the source of truth to check the
// files against, not the other way around.
//
// Four fatal verdicts:
//   - DROPPED          the manifest names a fixture identity no file under
//                       test/fixtures/derived/**/*.yaml actually declares
//                       (the corpus shrank silently, corpora.md's own
//                       named risk).
//   - UNDECLARED        a file exists under test/fixtures/derived/**/*.yaml
//                       whose `fixture:` identity the manifest never named
//                       (an ingest that bypassed the tool's own inventory).
//   - UNRECORDED        the manifest names a fixture identity absent from
//                       `test/fixtures/roster.json` — the same verdict name
//                       `kernel-fixture-loader.mjs` already uses for "passes
//                       but is unreviewed onto the roster," checked here
//                       against the tool's declared set directly, so it
//                       fires even before the loader ever compiles
//                       anything.
//   - VERSION-MISMATCH  a fixture's `provenance.derived_by` is not exactly
//                       `<manifest.tool>@<manifest.tool_version>` — the
//                       Goal's flat "the tool's version is in every
//                       provenance" rule, made mechanically checkable
//                       rather than left as an unenforced convention.
import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { join, relative } from "node:path";
import { parse } from "yaml";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const DERIVED_DIR = join(root, "test", "fixtures", "derived");
const MANIFEST_PATH = join(DERIVED_DIR, "manifest.json");
const ROSTER_PATH = join(root, "test", "fixtures", "roster.json");

function listYamlFiles(dir) {
  if (!existsSync(dir)) return [];
  const out = [];
  const walk = (d) => {
    for (const entry of readdirSync(d).sort()) {
      const full = join(d, entry);
      const st = statSync(full);
      if (st.isDirectory()) walk(full);
      else if (entry.endsWith(".yaml") || entry.endsWith(".yml")) out.push(full);
    }
  };
  walk(dir);
  return out;
}

function fail(violations) {
  console.error("derivation-sync: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}

if (!existsSync(MANIFEST_PATH)) {
  fail([`${relative(root, MANIFEST_PATH)} does not exist — nothing records what the derivation repository dropped here`]);
}

let manifest;
try {
  manifest = JSON.parse(readFileSync(MANIFEST_PATH, "utf8"));
} catch (e) {
  fail([`${relative(root, MANIFEST_PATH)}: could not parse JSON (${e.message})`]);
}

const structural = [];
if (typeof manifest.tool !== "string" || manifest.tool.length === 0) {
  structural.push(`manifest.tool must be a non-empty string`);
}
if (typeof manifest.tool_version !== "string" || manifest.tool_version.length === 0) {
  structural.push(`manifest.tool_version must be a non-empty string`);
}
if (!Array.isArray(manifest.fixtures)) {
  structural.push(`manifest.fixtures must be an array of fixture identities`);
}
if (structural.length > 0) fail(structural);

// The Goal's own rule, made a single string every fixture's own
// `provenance.derived_by` must match exactly — see docs/testing/registry-and-fixtures.md's
// fixture format, whose own example spells this same shape `derived_by:
// upstream-script-derive@1.4`.
const expectedDerivedBy = `${manifest.tool}@${manifest.tool_version}`;
const manifestSet = new Set(manifest.fixtures);

const violations = [];
const actualIds = new Set();

// Same "read the fixture's own `fixture:` field, never its file name"
// posture `kernel-fixture-loader.mjs` already established for identity.
for (const file of listYamlFiles(DERIVED_DIR)) {
  const rel = relative(root, file);
  let doc;
  try {
    doc = parse(readFileSync(file, "utf8")) ?? {};
  } catch (e) {
    violations.push(`${rel}: could not parse YAML (${e.message})`);
    continue;
  }
  if (!doc.fixture) {
    violations.push(`${rel}: missing required "fixture" identity field`);
    continue;
  }
  actualIds.add(doc.fixture);

  const derivedBy = doc.provenance && doc.provenance.derived_by;
  if (derivedBy !== expectedDerivedBy) {
    violations.push(
      `VERSION-MISMATCH: ${rel} ("${doc.fixture}") has provenance.derived_by ${JSON.stringify(derivedBy ?? null)}, expected "${expectedDerivedBy}" (manifest.tool@manifest.tool_version)`,
    );
  }
}

for (const id of manifestSet) {
  if (!actualIds.has(id)) {
    violations.push(`DROPPED: manifest declares "${id}" but no test/fixtures/derived/**/*.yaml fixture carries that identity`);
  }
}
for (const id of actualIds) {
  if (!manifestSet.has(id)) {
    violations.push(`UNDECLARED: "${id}" exists under test/fixtures/derived but the manifest never declared it`);
  }
}

const roster = existsSync(ROSTER_PATH) ? JSON.parse(readFileSync(ROSTER_PATH, "utf8")) : [];
const rosterSet = new Set(roster);
for (const id of manifestSet) {
  if (!rosterSet.has(id)) {
    violations.push(`UNRECORDED: "${id}" is in the derivation manifest but not yet in ${relative(root, ROSTER_PATH)}`);
  }
}

if (violations.length > 0) fail(violations);
console.log(`derivation-sync: PASS (${manifestSet.size} manifest fixture(s), tool ${expectedDerivedBy})`);
