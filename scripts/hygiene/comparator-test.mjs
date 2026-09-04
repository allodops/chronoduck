#!/usr/bin/env bun
// Compiles and runs test/kernel/comparator_test.cpp — the L1a direct test
// for src/kernel/comparator.hpp — with a bare g++, and fails if it fails to
// compile, exits nonzero, or doesn't print its own "PASS" sentinel. This is
// what makes the comparator's direct test "provably executed by an
// unconditional, failure-propagating lane" (Article V.5): `make hygiene`
// runs in the required "hygiene" CI job on every PR, and this scan is one of
// its checks, exactly like every other scripts/hygiene/*.mjs scan.
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const HERE = dirname(fileURLToPath(import.meta.url));
const testPath = join(root, "test", "kernel", "comparator_test.cpp");

async function run(cmd) {
  const proc = Bun.spawn(cmd, { stdout: "pipe", stderr: "pipe" });
  const [out, err, code] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
    proc.exited,
  ]);
  return { out, err, code };
}

const tmp = mkdtempSync(join(tmpdir(), "comparator-test-"));
const binPath = join(tmp, "comparator_test");

const compile = await run(["g++", "-std=c++17", "-Wall", "-Wextra", testPath, "-o", binPath]);
if (compile.code !== 0) {
  console.error("comparator-test: FAIL");
  console.error(`  test/kernel/comparator_test.cpp failed to compile:\n${compile.out}${compile.err}`);
  process.exit(1);
}

const ran = await run([binPath]);
if (ran.code !== 0 || !ran.out.includes("comparator_test: PASS")) {
  console.error("comparator-test: FAIL");
  console.error(`  test/kernel/comparator_test.cpp did not pass:\n${ran.out}${ran.err}`);
  process.exit(1);
}
console.log("comparator-test: PASS");
