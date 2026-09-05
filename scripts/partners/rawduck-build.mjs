#!/usr/bin/env bun
// make partner-rawduck-build (L15, issue #47)
//
// Fetches the commit pinned in scripts/partners/rawduck.json into a plain git
// clone at build/partners/rawduck/ — never a submodule of this repo, never
// committed (build/ is gitignored) — re-points that clone's own duckdb
// submodule at THIS repo's own pinned DuckDB tag (scripts/lib/duckdb-pin.mjs,
// not rawduck.json's informational duckdb_ref: layout parity requires both
// extensions to build against the identical DuckDB version), and builds
// rawduck.duckdb_extension with the same CMAKE_BUILD_PARALLEL_LEVEL
// convention the root Makefile exports for our own build.
//
// Caching: the built artifact is cached under
// build/partners/rawduck-cache/<key>/rawduck.duckdb_extension, keyed by a
// 16-hex-char slice of sha256("<partner commit>:<our duckdb pin>") — the
// only two inputs that can change what the build produces. A repeat run with
// both unchanged finds the cache populated and skips straight to copying the
// artifact back into the checkout's own build/ tree; either a new partner
// commit or a bumped duckdb-pin.mjs constant changes the key and forces a
// real rebuild.
//
// HEAD mode (`partner-rawduck-head`, L15, issue #49): with RAWDUCK_REF=head
// in the environment, the target commit is resolved at run time as
// scripts/partners/rawduck.json's repository's own default-branch HEAD via
// `git ls-remote`, instead of the file's pinned "commit" field — everything
// else (re-pointing the duckdb submodule at our own pin, the build, the
// cache) is identical. HEAD mode uses its own checkout
// (build/partners/rawduck-head/) and cache root
// (build/partners/rawduck-head-cache/) so it never disturbs the pinned
// checkout `make check-pins` compares scripts/partners/rawduck.json against.
import { $ } from "bun";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, copyFileSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { EXPECTED_DUCKDB_REF } from "../lib/duckdb-pin.mjs";

const HEAD_MODE = process.env.RAWDUCK_REF === "head";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const CONFIG_PATH = join(ROOT, "scripts", "partners", "rawduck.json");
const CHECKOUT_DIR = join(ROOT, "build", "partners", HEAD_MODE ? "rawduck-head" : "rawduck");
const CACHE_ROOT = join(ROOT, "build", "partners", HEAD_MODE ? "rawduck-head-cache" : "rawduck-cache");
const ARTIFACT_REL = join("build", "release", "extension", "rawduck", "rawduck.duckdb_extension");
const LABEL = HEAD_MODE ? "partner-rawduck-head-build" : "partner-rawduck-build";

function fail(message) {
  console.error(`${LABEL}: FAIL — ${message}`);
  process.exit(1);
}

async function run(cwd, cmd, env = {}) {
  const proc = Bun.spawn(cmd, { cwd, stdout: "pipe", stderr: "pipe", env: { ...process.env, ...env } });
  const [out, err, code] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
    proc.exited,
  ]);
  return { out, err, code };
}

if (!existsSync(CONFIG_PATH)) fail(`${CONFIG_PATH} does not exist`);
const config = JSON.parse(readFileSync(CONFIG_PATH, "utf8"));
if (!config.repository || !config.commit) {
  fail(`${CONFIG_PATH} must declare "repository" and "commit"`);
}

const repoUrl = `https://github.com/${config.repository}.git`;

// In HEAD mode, resolve the partner's own default-branch tip via a
// network-only `git ls-remote` — no local clone required to find it, and it
// reflects whatever the partner's default branch actually points at *right
// now*, which is the whole point of this lane (issue #49: catch upstream
// drift before it silently breaks compatibility).
async function resolveDefaultBranchHead() {
  const { out, err, code } = await run(ROOT, ["git", "ls-remote", repoUrl, "HEAD"]);
  if (code !== 0 || !out.trim()) {
    fail(`could not resolve ${config.repository}'s default-branch HEAD via \`git ls-remote ${repoUrl} HEAD\`:\n${err}`);
  }
  const sha = out.trim().split(/\s+/)[0];
  if (!/^[0-9a-f]{40}$/.test(sha)) {
    fail(`\`git ls-remote ${repoUrl} HEAD\` returned an unexpected line: "${out.trim()}"`);
  }
  return sha;
}

const targetCommit = HEAD_MODE ? await resolveDefaultBranchHead() : config.commit;
if (HEAD_MODE) {
  console.log(`${LABEL}: HEAD mode — ${config.repository}'s current default-branch HEAD is ${targetCommit}`);
}

const cacheKey = createHash("sha256").update(`${targetCommit}:${EXPECTED_DUCKDB_REF}`).digest("hex").slice(0, 16);
const cacheDir = join(CACHE_ROOT, cacheKey);
const cachedArtifact = join(cacheDir, "rawduck.duckdb_extension");

mkdirSync(dirname(CHECKOUT_DIR), { recursive: true });

// 1. Ensure the checkout dir is at exactly targetCommit (the pinned commit,
// or — in HEAD mode — the default-branch tip resolved above).
async function currentHead() {
  const { out, code } = await run(CHECKOUT_DIR, ["git", "rev-parse", "HEAD"]);
  return code === 0 ? out.trim() : null;
}

if (!existsSync(join(CHECKOUT_DIR, ".git"))) {
  console.log(`${LABEL}: cloning ${repoUrl} into ${CHECKOUT_DIR}`);
  const clone = await run(ROOT, ["git", "clone", repoUrl, CHECKOUT_DIR]);
  if (clone.code !== 0) {
    fail(`could not clone ${repoUrl}:\n${clone.err}`);
  }
} else {
  const head = await currentHead();
  if (head === null) {
    fail(`${CHECKOUT_DIR} exists but is not a usable git checkout — remove it and re-run`);
  }
}

let head = await currentHead();
if (head !== targetCommit) {
  console.log(`${LABEL}: checking out ${targetCommit} (currently at ${head})`);
  let checkout = await run(CHECKOUT_DIR, ["git", "checkout", "--detach", targetCommit]);
  if (checkout.code !== 0) {
    // the commit may not be reachable from whatever refs were fetched at clone time
    const fetch = await run(CHECKOUT_DIR, ["git", "fetch", "origin", targetCommit]);
    if (fetch.code === 0) {
      checkout = await run(CHECKOUT_DIR, ["git", "checkout", "--detach", targetCommit]);
    }
  }
  if (checkout.code !== 0) {
    fail(
      HEAD_MODE
        ? `could not check out ${config.repository}'s own default-branch HEAD "${targetCommit}" — has it moved again since resolution?\n${checkout.err}`
        : `could not check out pinned commit "${targetCommit}" in ${config.repository} — is scripts/partners/rawduck.json's "commit" field wrong?\n${checkout.err}`
    );
  }
  head = await currentHead();
  if (head !== targetCommit) {
    fail(`checkout of "${targetCommit}" reported success but HEAD is "${head}" — refusing to proceed`);
  }
}
console.log(
  `${LABEL}: ${CHECKOUT_DIR} is at ${HEAD_MODE ? `${config.repository}'s current default-branch HEAD` : "the pinned commit"} ${targetCommit}`
);

// 2. Cache hit: skip straight to placing the cached artifact and reporting.
if (existsSync(cachedArtifact)) {
  const destDir = join(CHECKOUT_DIR, dirname(ARTIFACT_REL));
  mkdirSync(destDir, { recursive: true });
  copyFileSync(cachedArtifact, join(CHECKOUT_DIR, ARTIFACT_REL));
  console.log(`${LABEL}: PASS (cache hit, key ${cacheKey}) — HEAD ${targetCommit} — ${join(CHECKOUT_DIR, ARTIFACT_REL)}`);
  process.exit(0);
}

// 3. Submodules: extension-ci-tools stays at the partner's own pin (build
// tooling only); duckdb is re-pointed at our own pin regardless of what the
// partner's .gitmodules or gitlink says — that re-pointing IS the point of
// this harness (layout parity requires the identical DuckDB version).
const ciTools = await run(CHECKOUT_DIR, ["git", "submodule", "update", "--init", "--", "extension-ci-tools"]);
if (ciTools.code !== 0) {
  fail(`could not init ${config.repository}'s own extension-ci-tools submodule:\n${ciTools.err}`);
}

const duckdbInit = await run(CHECKOUT_DIR, ["git", "submodule", "update", "--init", "--", "duckdb"]);
if (duckdbInit.code !== 0) {
  fail(`could not init ${config.repository}'s own duckdb submodule:\n${duckdbInit.err}`);
}
const duckdbDir = join(CHECKOUT_DIR, "duckdb");
let duckdbCheckout = await run(duckdbDir, ["git", "checkout", EXPECTED_DUCKDB_REF]);
if (duckdbCheckout.code !== 0) {
  const duckdbFetch = await run(duckdbDir, ["git", "fetch", "origin", "tag", EXPECTED_DUCKDB_REF]);
  if (duckdbFetch.code === 0) {
    duckdbCheckout = await run(duckdbDir, ["git", "checkout", EXPECTED_DUCKDB_REF]);
  }
}
if (duckdbCheckout.code !== 0) {
  fail(`could not re-point ${config.repository}'s duckdb submodule at our own pin "${EXPECTED_DUCKDB_REF}":\n${duckdbCheckout.err}`);
}
console.log(`${LABEL}: re-pointed ${duckdbDir} to our own pin ${EXPECTED_DUCKDB_REF}`);

// 4. Build, matching the root Makefile's own CMAKE_BUILD_PARALLEL_LEVEL convention.
const nproc = (await run(ROOT, ["nproc"])).out.trim() || "1";
console.log(`${LABEL}: building rawduck.duckdb_extension (commit ${targetCommit}, duckdb ${EXPECTED_DUCKDB_REF})`);
const build = await run(CHECKOUT_DIR, ["make", "release"], { CMAKE_BUILD_PARALLEL_LEVEL: nproc });
if (build.code !== 0) {
  const tail = (build.out + build.err).split("\n").slice(-60).join("\n");
  fail(
    `rawduck failed to build at ${HEAD_MODE ? `${config.repository}'s current default-branch HEAD commit` : "partner commit"} "${targetCommit}" against our duckdb pin "${EXPECTED_DUCKDB_REF}" (exit ${build.code}):\n${tail}`
  );
}

const artifactPath = join(CHECKOUT_DIR, ARTIFACT_REL);
if (!existsSync(artifactPath)) {
  fail(`build exited 0 but ${ARTIFACT_REL} was not produced (HEAD ${targetCommit})`);
}

// 5. Populate the cache for the next run with this same (commit, our pin).
mkdirSync(cacheDir, { recursive: true });
copyFileSync(artifactPath, cachedArtifact);

console.log(`${LABEL}: PASS (built, cached under key ${cacheKey}) — HEAD ${targetCommit} — ${artifactPath}`);
