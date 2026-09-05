#!/usr/bin/env bun
import { $ } from "bun";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
// Paths are relative to this file's own directory (scripts/), not implicitly
// rooted under scripts/hygiene/ — most scans live there, but fixtures-validate
// (#194) lives at scripts/fixtures-validate.mjs, schema.json's sibling per
// #146, so it's named without the "hygiene/" prefix like every other entry.
const TREE_SCANS = [
  "hygiene/forbid-ledger",
  "hygiene/forbid-consumer",
  "hygiene/verify-citations",
  "hygiene/workflow-shape",
  "hygiene/constitution-check",
  "hygiene/registry-closure",
  "hygiene/forbid-test-tolerance",
  "hygiene/kernel-primitive-tests",
  "hygiene/kernel-fixture-loader",
  "fixtures-validate",
];

let failed = false;
for (const scan of TREE_SCANS) {
  const proc = Bun.spawn(["bun", join(HERE, `${scan}.mjs`)], { stdout: "pipe", stderr: "pipe" });
  const [out, err, code] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
    proc.exited,
  ]);
  process.stdout.write(out);
  process.stderr.write(err);
  if (code !== 0) failed = true;
}

if (failed) {
  console.error("hygiene: FAIL (one or more scans failed above)");
  process.exit(1);
}
console.log("hygiene: PASS");
