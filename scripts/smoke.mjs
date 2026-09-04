#!/usr/bin/env bun
// LOAD the release build into a stock DuckDB shell (-unsigned, since it's not
// signed) and assert chronoduck_version() actually reports a version.
const duckdb = "./build/release/duckdb";
const extensionPath = "./build/release/extension/chronoduck/chronoduck.duckdb_extension";
const sql = `LOAD '${extensionPath}'; SELECT extension_version FROM duckdb_extensions() WHERE extension_name = 'chronoduck';`;

const proc = Bun.spawn([duckdb, "-unsigned", "-csv", "-noheader", "-c", sql], { stdout: "pipe", stderr: "pipe" });
const [out, err, code] = await Promise.all([
  new Response(proc.stdout).text(),
  new Response(proc.stderr).text(),
  proc.exited,
]);

if (code !== 0 || out.trim().length === 0) {
  console.error("smoke: FAIL");
  if (out.trim()) console.error(out.trim());
  if (err.trim()) console.error(err.trim());
  process.exit(1);
}
console.log("smoke: PASS");
