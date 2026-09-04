#!/usr/bin/env bun
import { $ } from "bun";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const TREE_SCANS = ["forbid-ledger", "forbid-consumer", "verify-citations", "workflow-shape", "constitution-check"];

let failed = false;
for (const scan of TREE_SCANS) {
  const proc = Bun.spawn(["bun", join(HERE, "hygiene", `${scan}.mjs`)], { stdout: "pipe", stderr: "pipe" });
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
