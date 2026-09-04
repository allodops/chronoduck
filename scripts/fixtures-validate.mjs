#!/usr/bin/env bun
// make fixtures-validate
// Validates every fixture under test/fixtures/*.yaml against the shape
// docs/testing/registry-and-fixtures.md defines (test/fixtures/schema.json
// is the canonical field reference; this script implements the checks
// directly rather than a generic JSON-Schema interpreter, per Article
// IV.2's dependency-light-script preference). EDGE_MODES, DOMAINS and
// REQUIRED_TOP_LEVEL are read from schema.json at runtime (a local
// JSON.parse, not a schema-interpreter library) rather than kept as a
// hardcoded parallel copy of the same lists. Two rules beyond plain
// structural validation: the inert-fixture rule (samples with no expected
// is red — a fixture that asserts nothing isn't a fixture), and that a
// forbidden-language token is only ever a violation in a fixture *key* or
// its `fixture`/`function` values, never a provenance *value*
// (scripts/hygiene/forbid-consumer.mjs already enforces that scoping; this
// script does not duplicate that scan).
import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { join, relative, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { parse } from "yaml";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

// schema.json describes this script's own contract, not the tree under
// --root — always read it from this script's real location so a
// materialized selftest fixture directory (which never includes a copy of
// it) still validates against the one canonical schema.
const HERE = dirname(fileURLToPath(import.meta.url));
const SCHEMA = JSON.parse(readFileSync(join(HERE, "..", "test", "fixtures", "schema.json"), "utf8"));

const EDGE_MODES = SCHEMA.properties.edge_mode.enum;
const DOMAINS = SCHEMA.properties.domain.enum;
const REQUIRED_TOP_LEVEL = SCHEMA.required;
const HISTOGRAM_LITERAL = SCHEMA.histogramLiteral;

// Per docs/testing/registry-and-fixtures.md: "(t, v) or (t, v, st); NaN and
// "stale" are literal tokens" — a sample's value may always be one of these
// two literal strings, regardless of domain, in place of a domain-shaped
// value.
const SAMPLE_LITERAL_TOKENS = ["NaN", "stale"];

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

// A minimal, hand-written matcher against the small subset of JSON-Schema
// vocabulary schema.json actually uses (type + nested properties for plain
// objects) — not a general interpreter, just enough to let histogram
// literal validation walk schema.json's own histogramLiteral definition
// instead of hardcoding a second copy of its field list.
function matchesType(value, node) {
  if (!node || !node.type) return true;
  switch (node.type) {
    case "number":
      return typeof value === "number" && Number.isFinite(value);
    case "integer":
      return Number.isInteger(value);
    case "string":
      return typeof value === "string";
    case "array":
      return Array.isArray(value);
    case "object": {
      if (value === null || typeof value !== "object" || Array.isArray(value)) return false;
      if (node.properties) {
        for (const [key, val] of Object.entries(value)) {
          if (node.properties[key] && !matchesType(val, node.properties[key])) return false;
        }
      }
      return true;
    }
    default:
      return true;
  }
}

function validateHistogramLiteral(value, rel, idx) {
  const violations = [];
  if (!matchesType(value, HISTOGRAM_LITERAL)) {
    violations.push(`${rel}: samples[${idx}] value is not a valid histogram literal object for domain "HISTOGRAM", got ${JSON.stringify(value)}`);
    return violations;
  }
  if (value && typeof value === "object" && "schema" in value && "custom_bounds" in value) {
    violations.push(`${rel}: samples[${idx}] histogram literal cannot declare both "schema" and "custom_bounds" (mutually exclusive per schema.json)`);
  }
  return violations;
}

function validateSampleValue(v, domain, rel, idx) {
  if (typeof v === "string" && SAMPLE_LITERAL_TOKENS.includes(v)) return [];
  if (domain === "HISTOGRAM") return validateHistogramLiteral(v, rel, idx);
  if (typeof v !== "number") {
    return [`${rel}: samples[${idx}] value must be a number for domain "${domain}" (or literal token ${SAMPLE_LITERAL_TOKENS.join("/")}), got ${JSON.stringify(v)}`];
  }
  return [];
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

  for (const field of ["window", "lookback"]) {
    const type = SCHEMA.properties[field].type;
    if (!matchesType(doc[field], SCHEMA.properties[field])) {
      violations.push(`${rel}: ${field} must be a ${type}, got ${JSON.stringify(doc[field])}`);
    }
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
        return;
      }
      violations.push(...validateSampleValue(s[1], doc.domain, rel, i));
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
