#!/usr/bin/env bun
// Compiles and runs every test/kernel/*_test.cpp — the L1a direct test for
// each dependency-free Tier 0-5 primitive translation unit under
// src/kernel/ — with a bare g++, and fails if any one of them fails to
// compile, exits nonzero, or doesn't print its own "<name>: PASS" sentinel.
// This is what makes each primitive's direct test "provably executed by an
// unconditional, failure-propagating lane" (Article V.5): `make hygiene`
// runs in the required "hygiene" CI job on every PR, and this scan is one of
// its checks, exactly like every other scripts/hygiene/*.mjs scan.
//
// Generalizes the single-file `comparator-test.mjs` this repo's #27 (the
// comparator) established, now that #28 (T1.4) adds four more
// dependency-free kernel TUs each with their own `_test.cpp` — one script,
// one compile-and-run convention, rather than one hardcoded script per
// primitive (Article II's own "single source of truth" spirit, applied to
// hygiene tooling rather than only to production code).
//
// #229: the compile invocation passes `-I <root>/src` so each test's own
// quoted `#include "kernel/<name>.hpp"` resolves the same way
// `src/chronoduck_extension.cpp` already resolves its own `#include
// "kernel/<name>.hpp"` — src/ is this tree's established header root, not
// the repo root — instead of via a fragile `../../src/kernel/<name>.hpp`
// climb out of test/kernel/. The fragile form is what let it compile with
// no -I flag at all in the first place. `-I <root>` is kept alongside it so
// `oracle_sweep_test.cpp`'s `#include "test/oracle/<name>.hpp"` (a
// cross-top-level-directory reference with no `src/chronoduck_extension.cpp`
// precedent to follow) keeps resolving root-relative.
// `forbid-relative-kernel-include.mjs` is the mechanical scan that keeps the
// fragile `../` form from coming back, regardless of which of these two
// clean forms a given include uses.
import { mkdtempSync, readdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";

const args = process.argv.slice(2);
const rootIdx = args.indexOf("--root");
const root = rootIdx === -1 ? process.cwd() : args[rootIdx + 1];

const KERNEL_TEST_DIR = join(root, "test", "kernel");

async function run(cmd) {
  const proc = Bun.spawn(cmd, { stdout: "pipe", stderr: "pipe" });
  const [out, err, code] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
    proc.exited,
  ]);
  return { out, err, code };
}

let files;
try {
  files = readdirSync(KERNEL_TEST_DIR)
    .filter((f) => f.endsWith("_test.cpp"))
    .sort();
} catch {
  console.error("kernel-primitive-tests: FAIL");
  console.error(`  ${KERNEL_TEST_DIR} does not exist`);
  process.exit(1);
}

if (files.length === 0) {
  console.error("kernel-primitive-tests: FAIL");
  console.error(`  no *_test.cpp files found under ${KERNEL_TEST_DIR}`);
  process.exit(1);
}

const tmp = mkdtempSync(join(tmpdir(), "kernel-primitive-tests-"));
const violations = [];

for (const file of files) {
  const testPath = join(KERNEL_TEST_DIR, file);
  const name = basename(file, ".cpp"); // e.g. "comparator_test"
  const binPath = join(tmp, name);

  const compile = await run([
    "g++",
    "-std=c++17",
    "-Wall",
    "-Wextra",
    "-I",
    join(root, "src"),
    "-I",
    root,
    testPath,
    "-o",
    binPath,
  ]);
  if (compile.code !== 0) {
    violations.push(`test/kernel/${file} failed to compile:\n${compile.out}${compile.err}`);
    continue;
  }

  const ran = await run([binPath]);
  const sentinel = `${name}: PASS`;
  if (ran.code !== 0 || !ran.out.includes(sentinel)) {
    violations.push(`test/kernel/${file} did not pass:\n${ran.out}${ran.err}`);
    continue;
  }
  console.log(`kernel-primitive-tests: PASS — test/kernel/${file}`);
}

if (violations.length > 0) {
  console.error("kernel-primitive-tests: FAIL");
  for (const v of violations) console.error(`  ${v}`);
  process.exit(1);
}
console.log("kernel-primitive-tests: PASS");
