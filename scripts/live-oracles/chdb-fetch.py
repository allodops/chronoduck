#!/usr/bin/env python3
# make chdb-fetch (#43, T2.5) -- vendors libchdb (chDB's in-process embeddable
# engine, https://github.com/chdb-io/chdb) the same way
# scripts/partners/rawduck-build.py vendors a storage partner: a pinned
# (repository, tag) in a checked-in JSON config (scripts/live-oracles/chdb.json),
# fetched into build/ (gitignored, never committed) and never a floating
# dependency. Where this differs from the rawduck precedent: chdb-core has no
# clonable-and-buildable source tree a plain `git clone` + `make` can turn
# into a shared library in CI time (it is ClickHouse's own build, which takes
# hours) -- chdb-core instead publishes prebuilt, checksummed platform tarballs
# as GitHub release assets, so "pinned" here means an exact tag *and* a
# pinned sha256 of the exact asset, verified on every fetch rather than
# trusted from the network.
#
# The one pinned asset already bundles libchdb.so together with the chdb.h/
# chdb.hpp C/C++ API headers built against that exact engine build, so this
# script vendors all three from the one checksummed download -- never a
# header sourced from a second, independently-versioned repository that
# could drift from the binary's real ABI.
import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import urllib.error
import urllib.request
from hashlib import sha256
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
CONFIG_PATH = HERE / "chdb.json"


def fail(message):
    print(f"chdb-fetch: FAIL — {message}", file=sys.stderr)
    sys.exit(1)


def current_platform_key():
    if sys.platform == "linux" and platform.machine() == "x86_64":
        return "linux-x86_64"
    return None


def vendoredChdbDir(config=None):
    if config is None:
        config = json.loads(CONFIG_PATH.read_text())
    return ROOT / "build" / "live-oracles" / "chdb" / config["tag"]


def ensureChdbVendored():
    if not CONFIG_PATH.exists():
        fail(f"{CONFIG_PATH} does not exist")
    config = json.loads(CONFIG_PATH.read_text())
    if not config.get("repository") or not config.get("tag"):
        fail(f'{CONFIG_PATH} must declare "repository" and "tag"')

    platform_key = current_platform_key()
    platforms = config.get("platforms") or {}
    if not platform_key or platform_key not in platforms:
        fail(
            f"no pinned libchdb asset for this platform ({sys.platform}/{platform.machine()}) in {CONFIG_PATH} — "
            f"the chdb-differential lane only runs on linux-x86_64 today; add a platform entry before running it elsewhere"
        )
    entry = platforms[platform_key]
    asset, expected_sha256 = entry["asset"], entry["sha256"]

    dest_dir = vendoredChdbDir(config)
    so_path = dest_dir / "libchdb.so"
    header_path = dest_dir / "chdb.h"
    if so_path.exists() and header_path.exists():
        print(f"chdb-fetch: already vendored at {dest_dir}")
        return {"dir": dest_dir, "so": so_path, "header": header_path}

    dest_dir.mkdir(parents=True, exist_ok=True)
    url = f"https://github.com/{config['repository']}/releases/download/{config['tag']}/{asset}"
    print(f"chdb-fetch: downloading {url}")
    try:
        with urllib.request.urlopen(url) as res:
            status = getattr(res, "status", 200)
            if not (200 <= status < 300):
                fail(f"GET {url} -> HTTP {status}")
            body = res.read()
    except urllib.error.HTTPError as e:
        fail(f"GET {url} -> HTTP {e.code}")

    actual_sha256 = sha256(body).hexdigest()
    if actual_sha256 != expected_sha256:
        fail(
            f"sha256 mismatch for {asset}: expected {expected_sha256}, got {actual_sha256} — "
            f"refusing to extract an asset that doesn't match the pin in {CONFIG_PATH}"
        )

    tmp_dir = Path(f"{dest_dir}.tmp-{os.getpid()}")
    shutil.rmtree(tmp_dir, ignore_errors=True)
    tmp_dir.mkdir(parents=True, exist_ok=True)
    tar_path = tmp_dir / asset
    tar_path.write_bytes(body)

    extract = subprocess.run(["tar", "-xzf", str(tar_path), "-C", str(tmp_dir)], capture_output=True, text=True)
    if extract.returncode != 0:
        fail(f"tar -xzf {asset} failed:\n{extract.stderr}")
    tar_path.unlink()

    for name in ["libchdb.so", "chdb.h", "chdb.hpp"]:
        if not (tmp_dir / name).exists():
            fail(f'{asset} did not contain expected file "{name}" — chdb-core\'s release layout may have changed')

    shutil.rmtree(dest_dir, ignore_errors=True)
    tmp_dir.rename(dest_dir)
    st = os.stat(so_path)
    os.chmod(so_path, st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    print(f"chdb-fetch: PASS (verified sha256, vendored {config['repository']}@{config['tag']} at {dest_dir})")
    return {"dir": dest_dir, "so": so_path, "header": header_path}


if __name__ == "__main__":
    ensureChdbVendored()
