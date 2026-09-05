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
import { $ } from "bun";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, copyFileSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { EXPECTED_DUCKDB_REF } from "../lib/duckdb-pin.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const CONFIG_PATH = join(ROOT, "scripts", "partners", "rawduck.json");
const CHECKOUT_DIR = join(ROOT, "build", "partners", "rawduck");
const CACHE_ROOT = join(ROOT, "build", "partners", "rawduck-cache");
const ARTIFACT_REL = join("build", "release", "extension", "rawduck", "rawduck.duckdb_extension");

function fail(message) {
  console.error(`partner-rawduck-build: FAIL — ${message}`);
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

const cacheKey = createHash("sha256").update(`${config.commit}:${EXPECTED_DUCKDB_REF}`).digest("hex").slice(0, 16);
const cacheDir = join(CACHE_ROOT, cacheKey);
const cachedArtifact = join(cacheDir, "rawduck.duckdb_extension");

mkdirSync(dirname(CHECKOUT_DIR), { recursive: true });

// 1. Ensure build/partners/rawduck/ is a checkout of exactly the pinned commit.
const repoUrl = `https://github.com/${config.repository}.git`;

async function currentHead() {
  const { out, code } = await run(CHECKOUT_DIR, ["git", "rev-parse", "HEAD"]);
  return code === 0 ? out.trim() : null;
}

if (!existsSync(join(CHECKOUT_DIR, ".git"))) {
  console.log(`partner-rawduck-build: cloning ${repoUrl} into build/partners/rawduck/`);
  const clone = await run(ROOT, ["git", "clone", repoUrl, CHECKOUT_DIR]);
  if (clone.code !== 0) {
    fail(`could not clone ${repoUrl}:\n${clone.err}`);
  }
} else {
  const head = await currentHead();
  if (head === null) {
    fail(`build/partners/rawduck/ exists but is not a usable git checkout — remove it and re-run`);
  }
}

let head = await currentHead();
if (head !== config.commit) {
  console.log(`partner-rawduck-build: checking out pinned commit ${config.commit} (currently at ${head})`);
  let checkout = await run(CHECKOUT_DIR, ["git", "checkout", "--detach", config.commit]);
  if (checkout.code !== 0) {
    // the commit may not be reachable from whatever refs were fetched at clone time
    const fetch = await run(CHECKOUT_DIR, ["git", "fetch", "origin", config.commit]);
    if (fetch.code === 0) {
      checkout = await run(CHECKOUT_DIR, ["git", "checkout", "--detach", config.commit]);
    }
  }
  if (checkout.code !== 0) {
    fail(
      `could not check out pinned commit "${config.commit}" in ${config.repository} — is scripts/partners/rawduck.json's "commit" field wrong?\n${checkout.err}`
    );
  }
  head = await currentHead();
  if (head !== config.commit) {
    fail(`checkout of "${config.commit}" reported success but HEAD is "${head}" — refusing to proceed`);
  }
}
console.log(`partner-rawduck-build: build/partners/rawduck/ is at the pinned commit ${config.commit}`);

// 2. Cache hit: skip straight to placing the cached artifact and reporting.
if (existsSync(cachedArtifact)) {
  const destDir = join(CHECKOUT_DIR, dirname(ARTIFACT_REL));
  mkdirSync(destDir, { recursive: true });
  copyFileSync(cachedArtifact, join(CHECKOUT_DIR, ARTIFACT_REL));
  console.log(`partner-rawduck-build: PASS (cache hit, key ${cacheKey}) — ${join(CHECKOUT_DIR, ARTIFACT_REL)}`);
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
console.log(`partner-rawduck-build: re-pointed build/partners/rawduck/duckdb to our own pin ${EXPECTED_DUCKDB_REF}`);

// 4. Build, matching the root Makefile's own CMAKE_BUILD_PARALLEL_LEVEL convention.
const nproc = (await run(ROOT, ["nproc"])).out.trim() || "1";
console.log(`partner-rawduck-build: building rawduck.duckdb_extension (commit ${config.commit}, duckdb ${EXPECTED_DUCKDB_REF})`);
const build = await run(CHECKOUT_DIR, ["make", "release"], { CMAKE_BUILD_PARALLEL_LEVEL: nproc });
if (build.code !== 0) {
  const tail = (build.out + build.err).split("\n").slice(-60).join("\n");
  fail(
    `rawduck failed to build at partner commit "${config.commit}" against our duckdb pin "${EXPECTED_DUCKDB_REF}" (exit ${build.code}):\n${tail}`
  );
}

const artifactPath = join(CHECKOUT_DIR, ARTIFACT_REL);
if (!existsSync(artifactPath)) {
  fail(`build exited 0 but ${ARTIFACT_REL} was not produced`);
}

// 5. Populate the cache for the next run with this same (commit, our pin).
mkdirSync(cacheDir, { recursive: true });
copyFileSync(artifactPath, cachedArtifact);

console.log(`partner-rawduck-build: PASS (built, cached under key ${cacheKey}) — ${artifactPath}`);
