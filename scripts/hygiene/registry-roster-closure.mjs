#!/usr/bin/env bun
// make registry-roster-closure — issue #36's second meta-test: "registry
// rows vs every roster" (docs/testing/registry-and-fixtures.md: "A row
// added to the registry with any of [the rosters L2 replay, the property
// roster, the partition test, the memory sentinel, the mutation phase map]
// missing fails at build or in L13, named after the row.").
//
// `scripts/hygiene/registry-closure.mjs` (#26) already fences the roster
// that exists at every milestone regardless of a row's shape — a
// `test/sql/<name>.test` file. This script fences the one other roster that
// is already mechanically checkable today without inventing new machinery
// this issue doesn't own: the L2 fixture roster
// (`docs/testing/registry-and-fixtures.md:` `by the fixture roster (each
// name must have L2 fixtures`). The remaining rosters that sentence names —
// property/ShapeID (no code yet), partition (per-row, tracked by each
// function's own issue, e.g. #35 for ts_rate), memory sentinel (L11,
// [RELEASE], not required for M1) — are out of this issue's stated scope,
// and the mutation phase map is this issue's own explicit "Out of scope:
// Mutation (M4)".
//
// Which rows the fixture roster applies to is a structural question, not a
// per-function allow-list (Article V.3's "there is no reason token, because
// a per-function exemption from a floor is an allow-list with a
// vocabulary" — the same principle applied here to L2 applicability rather
// than to the comparator): `test/fixtures/schema.json`'s `edge_mode` and
// `domain` fields are both required, non-empty enums that do not include
// `EDGE_NONE`/`DOMAIN_NONE` — so a row whose registry.def `domain` column is
// literally `DOMAIN_NONE` (today: `chronoduck_version`, `ts_grid_index`,
// `ts_grid` — a metadata function and two domain-agnostic GRID rows) cannot
// be expressed as a fixture at all, in this file format, regardless of its
// family. A row with any other `domain` value is fixture-representable and
// must have at least one `test/fixtures/**/*.yaml` fixture whose `function`
// field names it.
//
// The fixture's `function` field is language-unaware vocabulary (Article
// VI.3: "Registry vocabulary describes folds, edges, grids and value
// domains"), not the literal SQL-visible name: `docs/design/ecosystem.md`
// documents `ts_` as the extension's own SQL-function-prefix convention
// ("`ts_` as the function prefix, the same split DuckDB's spatial extension
// makes between its name and its `ST_` functions"), and
// `docs/design/surface.md`'s own registry sketch spells this row `rate`,
// not `ts_rate` — confirmed live in
// `test/fixtures/rate/reset-midwindow.yaml`, whose `function:` value is
// `rate`. A registry row is matched against fixtures by its name with any
// leading `ts_` stripped.
import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { join } from "node:path";
import { parse } from "yaml";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const REGISTRY_DEF_PATH = join("src", "kernel", "registry.def");
const FIXTURES_DIR = join(root, "test", "fixtures");

// name, family, state, det, edge_modes, domain, scale_kind — the same
// 7-column shape `src/kernel/registry.def`'s own header documents and
// `scripts/hygiene/registry-closure.mjs` already parses for column 1 alone;
// this scan additionally needs column 6 (domain).
const TS_FN_ROW_RE = /^\s*TS_FN\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)/gm;

function parseRegistryRows(root) {
  const defPath = join(root, REGISTRY_DEF_PATH);
  if (!existsSync(defPath)) return null;
  const content = readFileSync(defPath, "utf8");
  const rows = [];
  let m;
  TS_FN_ROW_RE.lastIndex = 0;
  while ((m = TS_FN_ROW_RE.exec(content))) {
    rows.push({ name: m[1], family: m[2], state: m[3], det: m[4], edgeModes: m[5], domain: m[6], scaleKind: m[7] });
  }
  return rows;
}

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

const rows = parseRegistryRows(root);
if (rows === null) {
  console.error("registry-roster-closure: FAIL");
  console.error(`  ${REGISTRY_DEF_PATH} does not exist`);
  process.exit(1);
}

const fixtureFunctions = new Set();
for (const file of listYamlFiles(FIXTURES_DIR)) {
  let doc;
  try {
    doc = parse(readFileSync(file, "utf8")) ?? {};
  } catch {
    continue; // scripts/fixtures-validate.mjs already fences malformed YAML
  }
  if (typeof doc.function === "string") fixtureFunctions.add(doc.function);
}

const violations = [];
for (const row of rows) {
  if (row.domain === "DOMAIN_NONE") continue; // structurally outside the fixture format — see header
  const bareName = row.name.replace(/^ts_/, "");
  if (!fixtureFunctions.has(bareName)) {
    violations.push(
      `row "${row.name}" declares domain=${row.domain} (fixture-representable) but no test/fixtures/**/*.yaml fixture has function: ${bareName}`,
    );
  }
}

if (violations.length > 0) {
  console.error("registry-roster-closure: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log(`registry-roster-closure: PASS (${rows.length} row(s), ${fixtureFunctions.size} distinct fixture function(s))`);
