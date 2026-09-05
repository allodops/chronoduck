#!/usr/bin/env bun
// make kernel-fixture-loader
// The L2 fixture-replay harness this issue's (#33) Goal names: "Loader runs
// every fixture with the comparator; roster with
// REGRESSED/VANISHED/ARRIVED-FAILING/UNRECORDED" (docs/testing/layers.md's
// L2 row, docs/testing/rules.md's T7). YAML parsing happens here (Bun
// already carries the `yaml` package for exactly this, per
// `fixtures-validate.mjs`'s own precedent — one YAML parser in the tree, not
// two); every number the fixture actually asserts is then checked by the
// real kernel headers, not a re-implementation of them:
// `test/kernel/rate_fixture_loader.cpp` (compiled once here with a bare
// g++, the same convention `scripts/hygiene/kernel-primitive-tests.mjs`
// established) composes `SampleBuffer`, `window_walk`, `counter_fold` and
// `extrapolate` and checks every grid point with
// `src/kernel/comparator.hpp`'s own `equal_values`.
//
// Scope: only fixtures with `function: rate`, `edge_mode: EXTRAPOLATE`,
// `domain: COUNTER` are replayed — the only composition
// `test/kernel/rate_fixture_eval.hpp` supports today (that header's own
// scope note). A fixture outside that today would be a hygiene-scan gap on
// its own terms (this script errors loudly rather than silently skipping),
// but the corpus this issue ships never exercises it.
//
// Identity ratchet (Article V.4 / T7): `test/fixtures/roster.json` is the
// set of fixture ids known to pass. Every fixture actually found under
// `test/fixtures/rate/*.yaml` is run; its id is then classified against the
// roster:
//   - in roster, passes now        -> OK
//   - in roster, fails now         -> REGRESSED
//   - in roster, missing entirely  -> VANISHED
//   - not in roster, fails         -> ARRIVED-FAILING
//   - not in roster, passes        -> UNRECORDED (add it to roster.json)
// All four are fatal (T7); a fixture set that matches the roster exactly,
// every one passing, is the only green outcome.
import { readFileSync, readdirSync, existsSync } from "node:fs";
import { join, relative, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { parse } from "yaml";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const FIXTURE_DIR = join(root, "test", "fixtures", "rate");
const ROSTER_PATH = join(root, "test", "fixtures", "roster.json");

// The evaluator's own C++ source is this script's machinery, not part of
// the tree under --root — always compiled from this script's real location,
// the same "read it from this script's real location" posture
// `fixtures-validate.mjs` documents for `schema.json`, so a materialized
// selftest root (which never includes a copy of it) still compiles the one
// real evaluator.
const REAL_HERE = dirname(fileURLToPath(import.meta.url));
const LOADER_CPP = join(REAL_HERE, "..", "..", "test", "kernel", "rate_fixture_loader.cpp");

const SUPPORTED_EDGE_MODE = "EXTRAPOLATE";
const SUPPORTED_DOMAIN = "COUNTER";

async function run(cmd, { input } = {}) {
  const proc = Bun.spawn(cmd, { stdout: "pipe", stderr: "pipe", stdin: input !== undefined ? "pipe" : undefined });
  if (input !== undefined) {
    proc.stdin.write(input);
    proc.stdin.end();
  }
  const [out, err, code] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
    proc.exited,
  ]);
  return { out, err, code };
}

function listFixtureFiles(dir) {
  if (!existsSync(dir)) return [];
  return readdirSync(dir)
    .filter((f) => f.endsWith(".yaml") || f.endsWith(".yml"))
    .sort()
    .map((f) => join(dir, f));
}

// [t, v] or [t, v, st] -> "t v has_st st", the wire format
// `rate_fixture_loader.cpp` reads. A non-numeric sample value (the `NaN`/
// `stale` literal tokens `docs/testing/registry-and-fixtures.md` allows) is
// out of this loader's scope — see this file's own header comment — and is
// reported as a loud error rather than silently coerced.
function sampleLine(sample, rel) {
  const [t, v, st] = sample;
  if (typeof v !== "number") {
    throw new Error(`${rel}: sample value ${JSON.stringify(v)} is not a plain number (NaN/stale samples are out of this loader's scope)`);
  }
  const hasSt = st !== undefined;
  return `${t} ${v} ${hasSt ? 1 : 0} ${hasSt ? st : 0}`;
}

function expectedLine(value) {
  return value === null || value === undefined ? "NULL" : String(value);
}

function buildWirePayload(doc, rel) {
  const lines = [];
  lines.push(`GRID ${doc.grid.start} ${doc.grid.end} ${doc.grid.step}`);
  lines.push(`WINDOW ${doc.window}`);
  lines.push(`NSAMPLES ${doc.samples.length}`);
  for (const s of doc.samples) lines.push(sampleLine(s, rel));
  lines.push(`NEXPECTED ${doc.expected.length}`);
  for (const e of doc.expected) lines.push(expectedLine(e));
  return lines.join("\n") + "\n";
}

async function compileLoader(tmpDir) {
  const binPath = join(tmpDir, "rate_fixture_loader");
  const compile = await run(["g++", "-std=c++17", "-Wall", "-Wextra", LOADER_CPP, "-o", binPath]);
  if (compile.code !== 0) {
    throw new Error(`failed to compile ${relative(root, LOADER_CPP)}:\n${compile.out}${compile.err}`);
  }
  return binPath;
}

async function evaluateFixture(binPath, doc, rel) {
  if (doc.function !== "rate") return null; // not this loader's concern
  if (doc.edge_mode !== SUPPORTED_EDGE_MODE || doc.domain !== SUPPORTED_DOMAIN) {
    throw new Error(
      `${rel}: edge_mode=${doc.edge_mode}/domain=${doc.domain} is outside this loader's scope (${SUPPORTED_EDGE_MODE}/${SUPPORTED_DOMAIN} only)`
    );
  }
  const payload = buildWirePayload(doc, rel);
  const result = await run([binPath], { input: payload });
  const pass = result.code === 0 && /^RESULT PASS$/m.test(result.out);
  return { pass, out: result.out, err: result.err };
}

async function main() {
  const { mkdtempSync } = await import("node:fs");
  const { tmpdir } = await import("node:os");
  const tmp = mkdtempSync(join(tmpdir(), "kernel-fixture-loader-"));
  const binPath = await compileLoader(tmp);

  const files = listFixtureFiles(FIXTURE_DIR);
  const current = new Map(); // fixture id -> { pass, rel }

  for (const file of files) {
    const rel = relative(root, file);
    const doc = parse(readFileSync(file, "utf8")) ?? {};
    if (!doc.fixture) {
      console.error(`kernel-fixture-loader: FAIL`);
      console.error(`  ${rel}: missing required "fixture" identity field`);
      process.exit(1);
    }
    const outcome = await evaluateFixture(binPath, doc, rel);
    if (outcome === null) continue; // not function: rate
    current.set(doc.fixture, { pass: outcome.pass, rel, diag: outcome.out + outcome.err });
  }

  const roster = existsSync(ROSTER_PATH) ? JSON.parse(readFileSync(ROSTER_PATH, "utf8")) : [];
  const rosterSet = new Set(roster);

  const violations = [];
  for (const id of rosterSet) {
    const entry = current.get(id);
    if (!entry) {
      violations.push(`VANISHED: "${id}" is in the roster but no current fixture declares that identity`);
    } else if (!entry.pass) {
      violations.push(`REGRESSED: "${id}" (${entry.rel}) is in the roster but fails now:\n${entry.diag}`);
    }
  }
  for (const [id, entry] of current) {
    if (rosterSet.has(id)) continue;
    if (entry.pass) {
      violations.push(`UNRECORDED: "${id}" (${entry.rel}) passes but is not yet in ${relative(root, ROSTER_PATH)}`);
    } else {
      violations.push(`ARRIVED-FAILING: "${id}" (${entry.rel}) is new and fails:\n${entry.diag}`);
    }
  }

  if (violations.length > 0) {
    console.error("kernel-fixture-loader: FAIL");
    for (const v of violations) console.error(`  ${v}`);
    process.exit(1);
  }
  console.log(`kernel-fixture-loader: PASS (${current.size} rate fixture(s), roster of ${rosterSet.size})`);
}

await main();
