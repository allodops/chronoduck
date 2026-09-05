#!/usr/bin/env bun
// make partner-rawduck-test (L15, issue #47)
//
// Loads chronoduck's own built extension AND the freshly built rawduck
// extension into the same stock DuckDB shell — build/release/duckdb, exactly
// the binary scripts/smoke.mjs already treats as "a stock DuckDB shell" for
// chronoduck's own LOAD, -unsigned since neither .duckdb_extension is signed
// — and runs every test/partners/rawduck/*.sql file against it.
//
// Smoke-LOAD only, per this issue's own scope (T2.9): confirms both
// extensions load together without conflict and a minimal RawDuck table
// answers a ts_rate query. Real fixture-driven layout-parity testing against
// RawDuck's on-disk layout is issue #48's job, not this script's.
//
// A *.sql file here is a plain SQL script, not a DuckDB sqllogictest file —
// deliberately not named `*.test`: DuckDB's own sqllogictest runner
// (`./build/release/test/unittest "test/*"`, what `make test` invokes)
// auto-discovers every `.test` file under `test/` regardless of directory
// and tries to parse it as sqllogictest syntax; a plain SQL script's `--`
// comment header isn't valid sqllogictest, so a `.test` extension here broke
// the ordinary test lane, not just this partner-specific one. Separately,
// sqllogictest's `require <extension>` directive resolves against DuckDB's
// own known-extension list, which a partner extension built out-of-tree
// under build/partners/rawduck/ is not part of, so the real DuckDB unittest
// runner could never load this file's contents even if the extension were
// `.test`. Executed the same way scripts/smoke.mjs executes its own
// one-liner: as a single `-c` script string against the CLI, success meaning
// exit 0 and no "Error:"-prefixed line in the output.
import { readFileSync, readdirSync, existsSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const DUCKDB_BIN = join(ROOT, "build", "release", "duckdb");
const CHRONODUCK_EXT = join(ROOT, "build", "release", "extension", "chronoduck", "chronoduck.duckdb_extension");
const RAWDUCK_ROOT = join(ROOT, "build", "partners", "rawduck");
const RAWDUCK_EXT = join(RAWDUCK_ROOT, "build", "release", "extension", "rawduck", "rawduck.duckdb_extension");
// RawDuck's own extension_config.cmake also builds DuckDB's core `json`
// extension as a sibling artifact (RawDuck's ingest path depends on the JSON
// logical type / to_json() being registered — see its README's "JSON
// extension: provides the JSON logical type and to_json()/json_* functions
// that RawDuck relies on for its structural-conflict columns"). LOADing it
// explicitly from the sibling artifact keeps this test hermetic — no
// reliance on DuckDB's online-autoinstall reaching the network in CI.
const RAWDUCK_JSON_EXT = join(RAWDUCK_ROOT, "build", "release", "extension", "json", "json.duckdb_extension");
const TEST_DIR = join(ROOT, "test", "partners", "rawduck");

function fail(message) {
  console.error(`partner-rawduck-test: FAIL — ${message}`);
  process.exit(1);
}

if (!existsSync(DUCKDB_BIN)) fail(`${DUCKDB_BIN} does not exist — run \`make release\` first`);
if (!existsSync(CHRONODUCK_EXT)) fail(`${CHRONODUCK_EXT} does not exist — run \`make release\` first`);
if (!existsSync(RAWDUCK_EXT)) fail(`${RAWDUCK_EXT} does not exist — run \`make partner-rawduck-build\` first`);
if (!existsSync(TEST_DIR)) fail(`${TEST_DIR} does not exist`);

const testFiles = readdirSync(TEST_DIR).filter((f) => f.endsWith(".sql")).sort();
if (testFiles.length === 0) fail(`no test/partners/rawduck/*.sql files found`);

const preamble = [
  existsSync(RAWDUCK_JSON_EXT) ? `LOAD '${RAWDUCK_JSON_EXT}';` : null,
  `LOAD '${CHRONODUCK_EXT}';`,
  `LOAD '${RAWDUCK_EXT}';`,
]
  .filter(Boolean)
  .join("\n");

let failures = 0;
for (const file of testFiles) {
  const script = readFileSync(join(TEST_DIR, file), "utf8");
  // A fresh scratch cwd per file: RawDuck's `ATTACH 'rawduck:<relative path>'`
  // creates a real on-disk store, which must never collide across test files
  // or across repeat runs of this same file.
  const cwd = mkdtempSync(join(tmpdir(), "partner-rawduck-test-"));
  const proc = Bun.spawn([DUCKDB_BIN, "-unsigned", "-c", `${preamble}\n${script}`], {
    cwd,
    stdout: "pipe",
    stderr: "pipe",
  });
  const [out, err, code] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
    proc.exited,
  ]);
  const combined = out + err;
  const erred = code !== 0 || /^Error:/m.test(combined);
  if (erred) {
    console.error(`partner-rawduck-test: FAIL — test/partners/rawduck/${file}`);
    console.error(combined.trim());
    failures++;
  } else {
    console.log(`partner-rawduck-test: PASS — test/partners/rawduck/${file}`);
  }
}

if (failures > 0) {
  console.error(`partner-rawduck-test: FAIL (${failures}/${testFiles.length} file(s))`);
  process.exit(1);
}
console.log(`partner-rawduck-test: PASS (${testFiles.length} file(s))`);
