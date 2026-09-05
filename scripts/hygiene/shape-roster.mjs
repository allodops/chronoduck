#!/usr/bin/env bun
// shape-roster — T7 (docs/testing/rules.md: "Ratchets gate on identity,
// never counts... Every roster — fixture IDs, ShapeIDs, partition schemes,
// mutation legs — records the set that must pass") applied to the L3
// property roster `docs/testing/registry-and-fixtures.md` names: "the
// property roster (each name x edge mode x value domain is a ShapeID)".
//
// A ShapeID is derived from `src/kernel/registry.def` itself — `name`
// (leading `ts_` stripped, matching `scripts/hygiene/registry-roster-closure.mjs`'s
// own convention for the fixture roster's `function:` field) x each
// `|`-separated `edge_modes` token x `domain`, skipping rows whose
// `edge_modes`/`domain` is `EDGE_NONE`/`DOMAIN_NONE` (a metadata or
// domain-agnostic row, like `chronoduck_version`/`ts_grid`/`ts_grid_index`,
// has no ShapeID to speak of — the same carve-out
// `registry-roster-closure.mjs` already applies to `DOMAIN_NONE` for the L2
// fixture roster).
//
// "Current" is a static citation scan, not a compiled/run proof (that proof
// is `scripts/hygiene/kernel-primitive-tests.mjs` compiling and running
// `test/kernel/oracle_sweep_test.cpp` for real, a separate concern): every
// `// ShapeID: <id>` comment directly above a worked example in
// `test/oracle/shape_examples.hpp` is this roster's "cited" set — the same
// citation-parsing convention `scripts/hygiene/tier-coverage-floor.mjs`
// already established for Tier/primitive coverage, applied here to
// ShapeIDs instead of Tier rows.
//
// Four fatal verdicts, mirroring `scripts/hygiene/kernel-fixture-loader.mjs`'s
// own roster-vs-current shape exactly, substituting "the registry still
// declares it AND it's cited" for "the fixture exists and passes":
//   - VANISHED:        in roster.json, but the registry no longer declares
//                       that (name, edge_mode, domain) combination at all.
//   - REGRESSED:       in roster.json and still a real registry shape, but
//                       no `// ShapeID:` citation exists for it any more.
//   - ARRIVED-FAILING: a real registry shape with NO citation at all and
//                       absent from roster.json (a new row with no worked
//                       example).
//   - UNRECORDED:      a real registry shape, cited, absent from
//                       roster.json — the fix here is recording it, not
//                       writing an example.
// A fifth, non-ratchet check catches a citation naming a shape the registry
// does not currently declare at all (a stale/typo citation) — UNKNOWN-SHAPE,
// the same idea as tier-coverage-floor.mjs's own UNKNOWN-PRIMITIVE.
import { readFileSync, existsSync } from "node:fs";
import { join } from "node:path";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const REGISTRY_DEF_PATH = join("src", "kernel", "registry.def");
const SHAPE_EXAMPLES_PATH = join(root, "test", "oracle", "shape_examples.hpp");
const ROSTER_PATH = join(root, "test", "oracle", "shape-roster.json");

// name, family, state, det, edge_modes, domain, scale_kind — the same
// 7-column shape registry-closure.mjs/registry-roster-closure.mjs already
// parse; `edge_modes` can be `|`-separated (docs/design/surface.md's own
// aspirational rows, e.g. `EXTRAPOLATE|ANCHOR|SMOOTH`), one ShapeID per mode.
const TS_FN_ROW_RE = /^\s*TS_FN\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_|][A-Za-z0-9_|]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)/gm;

function parseRegistryShapeIds(root) {
  const defPath = join(root, REGISTRY_DEF_PATH);
  if (!existsSync(defPath)) return null;
  const content = readFileSync(defPath, "utf8");
  const ids = new Set();
  let m;
  TS_FN_ROW_RE.lastIndex = 0;
  while ((m = TS_FN_ROW_RE.exec(content))) {
    const [, name, , , , edgeModes, domain] = m;
    if (edgeModes === "EDGE_NONE" || domain === "DOMAIN_NONE") continue;
    const bareName = name.replace(/^ts_/, "");
    for (const mode of edgeModes.split("|")) {
      ids.add(`${bareName}/${mode}/${domain}`);
    }
  }
  return ids;
}

// `// ShapeID: <name>/<EDGE_MODE>/<DOMAIN>` — one citation line, on its own,
// directly above each worked example in test/oracle/shape_examples.hpp.
// Anchored to the whole line and to the `name/EDGE_MODE/DOMAIN` shape
// itself (not a bare `\S+`) so this scan's own header-comment PROSE about
// the citation convention (which mentions the literal text "ShapeID:" too,
// as documentation) is never mistaken for a real citation.
const SHAPE_CITATION_RE = /^\/\/ ShapeID: ([A-Za-z_][A-Za-z0-9_]*\/[A-Z_]+\/[A-Z_]+)\s*$/gm;

function parseCitedShapeIds() {
  if (!existsSync(SHAPE_EXAMPLES_PATH)) return new Set();
  const content = readFileSync(SHAPE_EXAMPLES_PATH, "utf8");
  const ids = new Set();
  let m;
  SHAPE_CITATION_RE.lastIndex = 0;
  while ((m = SHAPE_CITATION_RE.exec(content))) {
    ids.add(m[1]);
  }
  return ids;
}

const registryShapes = parseRegistryShapeIds(root);
if (registryShapes === null) {
  console.error("shape-roster: FAIL");
  console.error(`  ${REGISTRY_DEF_PATH} does not exist`);
  process.exit(1);
}

const citedShapes = parseCitedShapeIds();
const roster = existsSync(ROSTER_PATH) ? JSON.parse(readFileSync(ROSTER_PATH, "utf8")) : [];
const rosterSet = new Set(roster);

const violations = [];

for (const id of rosterSet) {
  if (!registryShapes.has(id)) {
    violations.push(`VANISHED: "${id}" is in the roster but src/kernel/registry.def no longer declares that shape`);
  } else if (!citedShapes.has(id)) {
    violations.push(`REGRESSED: "${id}" is in the roster and still a real registry shape, but no "// ShapeID: ${id}" citation exists in test/oracle/shape_examples.hpp any more`);
  }
}

for (const id of registryShapes) {
  if (rosterSet.has(id)) continue;
  if (citedShapes.has(id)) {
    violations.push(`UNRECORDED: "${id}" has a worked example (test/oracle/shape_examples.hpp) but is not yet in ${relRoster()}`);
  } else {
    violations.push(`ARRIVED-FAILING: "${id}" is a registered (name, edge_mode, domain) shape with no worked example at all in test/oracle/shape_examples.hpp`);
  }
}

for (const id of citedShapes) {
  if (!registryShapes.has(id)) {
    violations.push(`UNKNOWN-SHAPE: test/oracle/shape_examples.hpp cites "${id}", which is not a (name, edge_mode, domain) combination src/kernel/registry.def currently declares`);
  }
}

function relRoster() {
  return "test/oracle/shape-roster.json";
}

if (violations.length > 0) {
  console.error("shape-roster: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log(`shape-roster: PASS (${rosterSet.size} shape(s) in the roster)`);
