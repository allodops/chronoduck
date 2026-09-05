#!/usr/bin/env bun
// make chdb-fetch (#43, T2.5) — vendors libchdb (chDB's in-process embeddable
// engine, https://github.com/chdb-io/chdb) the same way
// scripts/partners/rawduck-build.mjs vendors a storage partner: a pinned
// (repository, tag) in a checked-in JSON config (scripts/live-oracles/chdb.json),
// fetched into build/ (gitignored, never committed) and never a floating
// dependency. Where this differs from the rawduck precedent: chdb-core has no
// clonable-and-buildable source tree a plain `git clone` + `make` can turn
// into a shared library in CI time (it is ClickHouse's own build, which takes
// hours) — chdb-core instead publishes prebuilt, checksummed platform tarballs
// as GitHub release assets, so "pinned" here means an exact tag *and* a
// pinned sha256 of the exact asset, verified on every fetch rather than
// trusted from the network.
//
// The one pinned asset already bundles libchdb.so together with the chdb.h/
// chdb.hpp C/C++ API headers built against that exact engine build, so this
// script vendors all three from the one checksummed download — never a
// header sourced from a second, independently-versioned repository that
// could drift from the binary's real ABI.
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, rmSync, renameSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const CONFIG_PATH = join(HERE, "chdb.json");

function fail(message) {
  console.error(`chdb-fetch: FAIL — ${message}`);
  process.exit(1);
}

function currentPlatformKey() {
  if (process.platform === "linux" && process.arch === "x64") return "linux-x86_64";
  return null;
}

export function vendoredChdbDir(config = JSON.parse(readFileSync(CONFIG_PATH, "utf8"))) {
  return join(ROOT, "build", "live-oracles", "chdb", config.tag);
}

export async function ensureChdbVendored() {
  if (!existsSync(CONFIG_PATH)) fail(`${CONFIG_PATH} does not exist`);
  const config = JSON.parse(readFileSync(CONFIG_PATH, "utf8"));
  if (!config.repository || !config.tag) fail(`${CONFIG_PATH} must declare "repository" and "tag"`);

  const platformKey = currentPlatformKey();
  if (!platformKey || !config.platforms?.[platformKey]) {
    fail(
      `no pinned libchdb asset for this platform (${process.platform}/${process.arch}) in ${CONFIG_PATH} — ` +
        `the chdb-differential lane only runs on linux-x86_64 today; add a platform entry before running it elsewhere`
    );
  }
  const { asset, sha256: expectedSha256 } = config.platforms[platformKey];

  const destDir = vendoredChdbDir(config);
  const soPath = join(destDir, "libchdb.so");
  const headerPath = join(destDir, "chdb.h");
  if (existsSync(soPath) && existsSync(headerPath)) {
    console.log(`chdb-fetch: already vendored at ${destDir}`);
    return { dir: destDir, so: soPath, header: headerPath };
  }

  mkdirSync(destDir, { recursive: true });
  const url = `https://github.com/${config.repository}/releases/download/${config.tag}/${asset}`;
  console.log(`chdb-fetch: downloading ${url}`);
  const res = await fetch(url);
  if (!res.ok) fail(`GET ${url} -> HTTP ${res.status}`);
  const bytes = new Uint8Array(await res.arrayBuffer());

  const actualSha256 = createHash("sha256").update(bytes).digest("hex");
  if (actualSha256 !== expectedSha256) {
    fail(
      `sha256 mismatch for ${asset}: expected ${expectedSha256}, got ${actualSha256} — ` +
        `refusing to extract an asset that doesn't match the pin in ${CONFIG_PATH}`
    );
  }

  const tmpDir = `${destDir}.tmp-${process.pid}`;
  rmSync(tmpDir, { recursive: true, force: true });
  mkdirSync(tmpDir, { recursive: true });
  const tarPath = join(tmpDir, asset);
  await Bun.write(tarPath, bytes);

  const extract = Bun.spawnSync(["tar", "-xzf", tarPath, "-C", tmpDir]);
  if (extract.exitCode !== 0) {
    fail(`tar -xzf ${asset} failed:\n${extract.stderr?.toString() ?? ""}`);
  }
  rmSync(tarPath);

  for (const name of ["libchdb.so", "chdb.h", "chdb.hpp"]) {
    if (!existsSync(join(tmpDir, name))) {
      fail(`${asset} did not contain expected file "${name}" — chdb-core's release layout may have changed`);
    }
  }

  rmSync(destDir, { recursive: true, force: true });
  renameSync(tmpDir, destDir);
  Bun.spawnSync(["chmod", "+x", soPath]);

  console.log(`chdb-fetch: PASS (verified sha256, vendored ${config.repository}@${config.tag} at ${destDir})`);
  return { dir: destDir, so: soPath, header: headerPath };
}

if (import.meta.main) {
  await ensureChdbVendored();
}
