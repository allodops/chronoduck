#!/usr/bin/env bun
// make chdb-differential (#43, T2.5) — the L6a chDB leg of the SQL-substrate
// differential, on the merge gate (docs/testing/layers.md's L6a row: "[MERGE]
// (chDB)"). One driver, one back-end (chDB — the Timescale leg is M3, out of
// this issue's scope per its own "Out of scope" note), one fixture format:
// every rate fixture (`test/fixtures/rate/*.yaml` and
// `test/fixtures/derived/**/*.yaml`) either runs through
// `test/live_oracles/chdb/chdb_diff_eval.cpp` (compiled once here, the same
// "compile once, drive over stdin" shape `scripts/hygiene/kernel-fixture-loader.mjs`
// already established for the L2 leg) or is recorded as a ✗-by-shape roster
// gap when chDB's own `timeSeriesRateToGrid(ts, value)` signature has no
// argument for a bound start timestamp (`docs/testing/live-oracles.md`: "A
// fixture an oracle cannot evaluate at all ... is not a divergence; it is a
// roster gap").
//
// Scope: the same "rate fixture corpus" definition
// `scripts/hygiene/kernel-fixture-loader.mjs` already established for the L2
// leg — every fixture actually found under `test/fixtures/rate/*.yaml` OR
// `test/fixtures/derived/**/*.yaml`, read alongside each other rather than
// through a second, narrower scan. A fixture this leg cannot run through
// chDB's own signature is still a roster gap, exactly like the flat
// `test/fixtures/rate/` case; nothing about a fixture living under
// `derived/` changes that.
//
// Identity ratchet (Article V.4 / T7), generalised to three observed states
// instead of kernel-fixture-loader.mjs's two (pass/fail): a (fixture, oracle)
// pair is either a comparator PASS, a comparator FAIL, or a declared
// SHAPE_GAP. `test/fixtures/chdb-oracle-roster.json` records the "pass" set
// and the "shape_gap" set the current tree is known to produce; anything
// else is one of the four fatal verdicts:
//   - roster.pass entry, still PASS                    -> OK
//   - roster.pass entry, now FAIL or SHAPE_GAP          -> REGRESSED
//   - roster.pass entry, fixture no longer exists       -> VANISHED
//   - roster.shape_gap entry, still SHAPE_GAP            -> OK
//   - roster.shape_gap entry, now PASS (template gained  -> UNRECORDED
//     the capability — an improvement the roster doesn't record)
//   - roster.shape_gap entry, now FAIL                   -> ARRIVED-FAILING
//   - roster.shape_gap entry, fixture no longer exists   -> VANISHED
//   - not in either roster set, now PASS or SHAPE_GAP    -> UNRECORDED
//   - not in either roster set, now FAIL                 -> ARRIVED-FAILING
import { readFileSync, readdirSync, statSync, existsSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, relative, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { parse } from "yaml";
import { ensureChdbVendored } from "./chdb-fetch.mjs";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const FIXTURE_DIR = join(root, "test", "fixtures", "rate");
const DERIVED_DIR = join(root, "test", "fixtures", "derived");
const ROSTER_PATH = join(root, "test", "fixtures", "chdb-oracle-roster.json");
const ORACLE = "chdb";

const REAL_HERE = dirname(fileURLToPath(import.meta.url));
const EVAL_CPP = join(REAL_HERE, "..", "..", "test", "live_oracles", "chdb", "chdb_diff_eval.cpp");

// Which declared divergence (src/kernel/chdb_divergence.hpp) each fixture
// exercises, hand-mapped the same way `test/fixtures/rate/*.yaml`'s own
// `wrong:` sections name a specific bug rather than being inferred — a
// fixture's relationship to a *named* divergence is exactly the kind of
// thing a scan should not guess at. Absent here means `NONE`: the fixture
// and chDB are expected to agree with nothing to name.
const DIVERGENCE_BY_FIXTURE = {
  "rate/dup-duplicate-timestamp": "DUP_TS_KEEPS_MAX",
  "rate/threshold-single-sample-without-st": "NULL_FOR_TOO_FEW",
};

function fail(message) {
  console.error(`chdb-differential: FAIL — ${message}`);
  process.exit(1);
}

async function run(cmd, { input, env } = {}) {
  const proc = Bun.spawn(cmd, {
    stdout: "pipe",
    stderr: "pipe",
    stdin: input !== undefined ? "pipe" : undefined,
    env: env ? { ...process.env, ...env } : undefined,
  });
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

// Recursive, so `test/fixtures/derived/rate/*.yaml` (and any further
// per-source subdirectory a future batch adds) is found the same way
// `test/fixtures/rate/*.yaml`'s own flat listing already is — one walker,
// mirroring `scripts/hygiene/kernel-fixture-loader.mjs`'s own, not a second
// copy for the nested case.
function listFixtureFiles(dir) {
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

// chDB's own `timeSeriesRateToGrid(ts, value)` has no argument for a bound
// start timestamp — any 3-element `[t, v, st]` sample makes this fixture
// unrepresentable in chDB's signature at all (a roster gap, not a
// divergence).
function hasStartTimestamp(doc) {
  return doc.samples.some((s) => s.length >= 3);
}

function buildWirePayload(doc, divergenceTag) {
  const lines = [];
  lines.push(`GRID ${doc.grid.start} ${doc.grid.end} ${doc.grid.step}`);
  lines.push(`WINDOW ${doc.window}`);
  lines.push(`NSAMPLES ${doc.samples.length}`);
  for (const [t, v] of doc.samples) {
    if (typeof v !== "number") {
      throw new Error(`sample value ${JSON.stringify(v)} is not a plain number (NaN/stale samples are out of this leg's scope)`);
    }
    lines.push(`${t} ${v}`);
  }
  lines.push(`DIVERGENCE ${divergenceTag ?? "NONE"}`);
  return lines.join("\n") + "\n";
}

// #229/#235: chdb_diff_eval.cpp reaches its own kernel headers with a bare
// quoted include (`#include "kernel/comparator.hpp"`, ...) — the same
// `src/chronoduck_extension.cpp` convention `kernel-primitive-tests.mjs` and
// `kernel-fixture-loader.mjs` already resolve via `-I <root>/src` — and
// reaches `test/kernel/rate_fixture_eval.hpp` the same way via `-I
// <root>/test`, rather than the fragile three- and two-level `../` climbs
// this file used before #235's correction. Both flags are required: this
// leg was the one compile site #235 originally missed. `rate_fixture_eval.hpp`
// itself still resolved fine (a short, in-tree relative include), but once
// #235 rewrote its own nested includes to a clean, non-`../` form, this
// site's g++ invocation carried no -I flag at all to resolve them with —
// exactly the CI failure #235 shipped with.
async function compileEvaluator(tmpDir, chdbDir) {
  const binPath = join(tmpDir, "chdb_diff_eval");
  const compile = await run([
    "g++",
    "-std=c++17",
    "-O1",
    "-Wall",
    "-Wextra",
    `-I${join(root, "src")}`,
    `-I${join(root, "test")}`,
    `-I${chdbDir}`,
    EVAL_CPP,
    "-o",
    binPath,
    `-L${chdbDir}`,
    "-lchdb",
    `-Wl,-rpath,${chdbDir}`,
  ]);
  if (compile.code !== 0) {
    throw new Error(`failed to compile ${relative(root, EVAL_CPP)}:\n${compile.out}${compile.err}`);
  }
  return binPath;
}

async function main() {
  const { dir: chdbDir } = await ensureChdbVendored();
  const tmp = mkdtempSync(join(tmpdir(), "chdb-differential-"));
  const binPath = await compileEvaluator(tmp, chdbDir);

  const files = [...listFixtureFiles(FIXTURE_DIR), ...listFixtureFiles(DERIVED_DIR)];
  const current = new Map(); // "<fixture>@chdb" -> { state: "pass"|"fail"|"shape_gap", rel, diag }

  for (const file of files) {
    const rel = relative(root, file);
    const doc = parse(readFileSync(file, "utf8")) ?? {};
    if (!doc.fixture) fail(`${rel}: missing required "fixture" identity field`);
    if (doc.function !== "rate") continue; // this leg only carries the rate family today

    const key = `${doc.fixture}@${ORACLE}`;

    if (hasStartTimestamp(doc)) {
      current.set(key, { state: "shape_gap", rel, diag: "chDB's timeSeriesRateToGrid has no start-timestamp argument" });
      continue;
    }

    const payload = buildWirePayload(doc, DIVERGENCE_BY_FIXTURE[doc.fixture]);
    const result = await run([binPath], { input: payload, env: { LD_LIBRARY_PATH: chdbDir } });
    const diag = result.out + result.err;
    if (/^RESULT PASS$/m.test(result.out)) {
      current.set(key, { state: "pass", rel, diag });
    } else {
      current.set(key, { state: "fail", rel, diag });
    }
  }

  const roster = existsSync(ROSTER_PATH) ? JSON.parse(readFileSync(ROSTER_PATH, "utf8")) : { pass: [], shape_gap: [] };
  const rosterPass = new Set(roster.pass ?? []);
  const rosterGap = new Set(roster.shape_gap ?? []);

  const violations = [];

  for (const id of rosterPass) {
    const entry = current.get(id);
    if (!entry) {
      violations.push(`VANISHED: "${id}" is in the roster's pass set but no current fixture declares that identity`);
    } else if (entry.state !== "pass") {
      violations.push(`REGRESSED: "${id}" (${entry.rel}) was PASS but is now ${entry.state.toUpperCase()}:\n${entry.diag}`);
    }
  }
  for (const id of rosterGap) {
    const entry = current.get(id);
    if (!entry) {
      violations.push(`VANISHED: "${id}" is in the roster's shape_gap set but no current fixture declares that identity`);
    } else if (entry.state === "fail") {
      violations.push(`ARRIVED-FAILING: "${id}" (${entry.rel}) is a declared shape gap but now fails outright:\n${entry.diag}`);
    } else if (entry.state === "pass") {
      violations.push(`UNRECORDED: "${id}" (${entry.rel}) is a declared shape gap but now passes — update the roster`);
    }
  }
  for (const [id, entry] of current) {
    if (rosterPass.has(id) || rosterGap.has(id)) continue;
    if (entry.state === "fail") {
      violations.push(`ARRIVED-FAILING: "${id}" (${entry.rel}) is new and fails:\n${entry.diag}`);
    } else {
      violations.push(`UNRECORDED: "${id}" (${entry.rel}) is new and ${entry.state === "pass" ? "passes" : "is a shape gap"} — not yet in ${relative(root, ROSTER_PATH)}`);
    }
  }

  if (violations.length > 0) {
    console.error("chdb-differential: FAIL");
    for (const v of violations) console.error(`  ${v}`);
    process.exit(1);
  }

  const nPass = [...current.values()].filter((e) => e.state === "pass").length;
  const nGap = [...current.values()].filter((e) => e.state === "shape_gap").length;
  console.log(`chdb-differential: PASS (${nPass} fixture(s) compared against chDB, ${nGap} declared shape gap(s))`);
}

await main();
