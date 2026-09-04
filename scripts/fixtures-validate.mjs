#!/usr/bin/env bun
// make fixtures-validate
// Validates every fixture under test/fixtures/*.yaml against the shape
// docs/testing/registry-and-fixtures.md defines (test/fixtures/schema.json
// is the canonical field reference; this script implements the checks
// directly rather than a generic JSON-Schema interpreter, per Article
// IV.2's dependency-light-script preference). Two rules beyond plain
// structural validation: the inert-fixture rule (samples with no expected
// is red — a fixture that asserts nothing isn't a fixture), and that a
// forbidden-language token is only ever a violation in a fixture *key*,
// never a provenance *value* (scripts/hygiene/forbid-consumer.mjs already
// enforces the value/key scoping; this script does not duplicate that scan).
import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { join, relative } from "node:path";
import { parse } from "yaml";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const EDGE_MODES = ["INSIDE", "EXTRAPOLATE", "ANCHOR", "SMOOTH", "LOOKBACK"];
const DOMAINS = ["COUNTER", "GAUGE", "NONNEG", "ANY", "HISTOGRAM"];
const REQUIRED_TOP_LEVEL = ["fixture", "function", "edge_mode", "domain", "grid", "window", "lookback", "samples", "provenance"];

function listYamlFiles(dir) {
  if (!existsSync(dir)) return [];
  const out = [];
  const walk = (d) => {
    for (const entry of readdirSync(d)) {
      const full = join(d, entry);
      const st = statSync(full);
      if (st.isDirectory()) walk(full);
      else if (entry.endsWith(".yaml") || entry.endsWith(".yml")) out.push(full);
    }
  };
  walk(dir);
  return out;
}

function validateFixture(doc, rel) {
  const violations = [];

  for (const key of REQUIRED_TOP_LEVEL) {
    if (!(key in doc)) violations.push(`${rel}: missing required field "${key}"`);
  }
  if (violations.length > 0) return violations; // structural — no point checking further

  if (!EDGE_MODES.includes(doc.edge_mode)) {
    violations.push(`${rel}: edge_mode "${doc.edge_mode}" is not one of ${EDGE_MODES.join(", ")}`);
  }
  if (!DOMAINS.includes(doc.domain)) {
    violations.push(`${rel}: domain "${doc.domain}" is not one of ${DOMAINS.join(", ")}`);
  }

  const grid = doc.grid ?? {};
  for (const field of ["start", "end", "step"]) {
    if (typeof grid[field] !== "number") violations.push(`${rel}: grid.${field} must be a number`);
  }
  if (typeof grid.step === "number" && grid.step <= 0) {
    violations.push(`${rel}: grid.step must be positive, got ${grid.step}`);
  }

  if (!Array.isArray(doc.samples)) {
    violations.push(`${rel}: samples must be an array`);
  } else {
    doc.samples.forEach((s, i) => {
      if (!Array.isArray(s) || s.length < 2 || s.length > 3) {
        violations.push(`${rel}: samples[${i}] must be [t, v] or [t, v, st]`);
      }
    });
  }

  if (!doc.provenance || typeof doc.provenance !== "object" || !doc.provenance.source) {
    violations.push(`${rel}: provenance.source is required`);
  }

  // The inert-fixture rule: non-empty samples with no (or empty) expected
  // asserts nothing — a fixture with no assertion is dead weight, not a
  // fixture.
  const hasSamples = Array.isArray(doc.samples) && doc.samples.length > 0;
  const hasExpected = Array.isArray(doc.expected) && doc.expected.length > 0;
  if (hasSamples && !hasExpected) {
    violations.push(`${rel}: inert fixture — samples present but no expected`);
  }

  return violations;
}

const files = listYamlFiles(join(root, "test", "fixtures"));
const violations = [];

for (const file of files) {
  const rel = relative(root, file);
  let doc;
  try {
    doc = parse(readFileSync(file, "utf8"));
  } catch (e) {
    violations.push(`${rel}: could not parse YAML (${e.message})`);
    continue;
  }
  violations.push(...validateFixture(doc ?? {}, rel));
}

if (violations.length > 0) {
  console.error("fixtures-validate: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log(`fixtures-validate: PASS (${files.length} fixture(s))`);
