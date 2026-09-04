#!/usr/bin/env bun
import { $ } from "bun";
import { mkdtempSync, cpSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..");
const FIXTURES = join(ROOT, "test", "hygiene-fixtures");

async function run(cmd) {
  const proc = Bun.spawn(cmd, { stdout: "pipe", stderr: "pipe" });
  const [out, err, code] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
    proc.exited,
  ]);
  return { out, err, code };
}

let failures = 0;

async function expectRed(label, cmd) {
  const { out, err, code } = await run(cmd);
  if (code === 0) {
    console.error(`SELFTEST FAIL: ${label} was expected to fail (red) on its fixture but exited 0`);
    console.error(out, err);
    failures++;
  } else {
    console.log(`SELFTEST ok: ${label} correctly red on its fixture`);
  }
}

// 1-4: the filesystem-rooted tree scans.
await expectRed("forbid-ledger", ["bun", join(HERE, "hygiene", "forbid-ledger.mjs"), "--root", join(FIXTURES, "forbid-ledger")]);
await expectRed("forbid-consumer", ["bun", join(HERE, "hygiene", "forbid-consumer.mjs"), "--root", join(FIXTURES, "forbid-consumer")]);
await expectRed("verify-citations", ["bun", join(HERE, "hygiene", "verify-citations.mjs"), "--root", join(FIXTURES, "verify-citations")]);
await expectRed("workflow-shape", ["bun", join(HERE, "hygiene", "workflow-shape.mjs"), "--root", join(FIXTURES, "workflow-shape")]);

// 5: constitution-check needs a real git history — build one from the base/head fixture files.
{
  const tmp = mkdtempSync(join(tmpdir(), "cc-selftest-"));
  await $`git -C ${tmp} init -q -b main`.quiet();
  await $`git -C ${tmp} config user.email test@example.com`.quiet();
  await $`git -C ${tmp} config user.name test`.quiet();
  cpSync(join(FIXTURES, "constitution-check", "base", "CONSTITUTION.md"), join(tmp, "CONSTITUTION.md"));
  await $`git -C ${tmp} add CONSTITUTION.md`.quiet();
  await $`git -C ${tmp} commit -q -m base`.quiet();
  await $`git -C ${tmp} checkout -q -b head`.quiet();
  cpSync(join(FIXTURES, "constitution-check", "head", "CONSTITUTION.md"), join(tmp, "CONSTITUTION.md"));
  await $`git -C ${tmp} add CONSTITUTION.md`.quiet();
  await $`git -C ${tmp} commit -q -m "change without bumping version"`.quiet();
  await expectRed("constitution-check", ["bun", join(HERE, "hygiene", "constitution-check.mjs"), "--root", tmp, "--base", "main"]);
}

// 6: forbid-deferral, diff-based.
await expectRed("forbid-deferral", ["bun", join(HERE, "hygiene", "forbid-deferral.mjs"), "--diff-file", join(FIXTURES, "forbid-deferral", "diff.patch")]);

// 7: pr-hygiene, fixture-based (a PR body that pastes the issue body verbatim).
await expectRed("pr-hygiene", ["bun", join(HERE, "pr-hygiene.mjs"), "--fixture", join(FIXTURES, "pr-hygiene")]);

// Now the real tree must be green.
{
  const { out, err, code } = await run(["bun", join(HERE, "hygiene.mjs")]);
  process.stdout.write(out);
  process.stderr.write(err);
  if (code !== 0) {
    console.error("SELFTEST FAIL: `just hygiene` is not green on the real tree");
    failures++;
  } else {
    console.log("SELFTEST ok: `just hygiene` is green on the real tree");
  }
}

if (failures > 0) {
  console.error(`hygiene-selftest: FAIL (${failures} check(s))`);
  process.exit(1);
}
console.log("hygiene-selftest: PASS (all seven scans red on their fixtures, hygiene green on the tree)");
