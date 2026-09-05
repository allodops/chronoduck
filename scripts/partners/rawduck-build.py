#!/usr/bin/env python3
# make partner-rawduck-build (L15, issue #47)
#
# Fetches the commit pinned in scripts/partners/rawduck.json into a plain git
# clone at build/partners/rawduck/ -- never a submodule of this repo, never
# committed (build/ is gitignored) -- re-points that clone's own duckdb
# submodule at THIS repo's own pinned DuckDB tag (scripts/lib/duckdb_pin.py,
# not rawduck.json's informational duckdb_ref: layout parity requires both
# extensions to build against the identical DuckDB version), and builds
# rawduck.duckdb_extension with the same CMAKE_BUILD_PARALLEL_LEVEL
# convention the root Makefile exports for our own build.
#
# Caching: the built artifact is cached under
# build/partners/rawduck-cache/<key>/rawduck.duckdb_extension, keyed by a
# 16-hex-char slice of sha256("<partner commit>:<our duckdb pin>") -- the
# only two inputs that can change what the build produces. A repeat run with
# both unchanged finds the cache populated and skips straight to copying the
# artifact back into the checkout's own build/ tree; either a new partner
# commit or a bumped duckdb_pin.py constant changes the key and forces a
# real rebuild.
#
# HEAD mode (`partner-rawduck-head`, L15, issue #49): with RAWDUCK_REF=head
# in the environment, the target commit is resolved at run time as
# scripts/partners/rawduck.json's repository's own default-branch HEAD via
# `git ls-remote`, instead of the file's pinned "commit" field -- everything
# else (re-pointing the duckdb submodule at our own pin, the build, the
# cache) is identical. HEAD mode uses its own checkout
# (build/partners/rawduck-head/) and cache root
# (build/partners/rawduck-head-cache/) so it never disturbs the pinned
# checkout `make check-pins` compares scripts/partners/rawduck.json against.
import json
import os
import re
import shutil
import subprocess
import sys
from hashlib import sha256
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "lib"))
from duckdb_pin import EXPECTED_DUCKDB_REF  # noqa: E402

HEAD_MODE = os.environ.get("RAWDUCK_REF") == "head"

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
CONFIG_PATH = ROOT / "scripts" / "partners" / "rawduck.json"
CHECKOUT_DIR = ROOT / "build" / "partners" / ("rawduck-head" if HEAD_MODE else "rawduck")
CACHE_ROOT = ROOT / "build" / "partners" / ("rawduck-head-cache" if HEAD_MODE else "rawduck-cache")
ARTIFACT_REL = Path("build") / "release" / "extension" / "rawduck" / "rawduck.duckdb_extension"
LABEL = "partner-rawduck-head-build" if HEAD_MODE else "partner-rawduck-build"


def fail(message):
    print(f"{LABEL}: FAIL — {message}", file=sys.stderr)
    sys.exit(1)


def run(cwd, cmd, env=None):
    full_env = {**os.environ, **env} if env else None
    proc = subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True, env=full_env)
    return proc.stdout, proc.stderr, proc.returncode


if not CONFIG_PATH.exists():
    fail(f"{CONFIG_PATH} does not exist")
config = json.loads(CONFIG_PATH.read_text())
if not config.get("repository") or not config.get("commit"):
    fail(f'{CONFIG_PATH} must declare "repository" and "commit"')

repo_url = f"https://github.com/{config['repository']}.git"


# In HEAD mode, resolve the partner's own default-branch tip via a
# network-only `git ls-remote` -- no local clone required to find it, and it
# reflects whatever the partner's default branch actually points at *right
# now*, which is the whole point of this lane (issue #49: catch upstream
# drift before it silently breaks compatibility).
def resolve_default_branch_head():
    out, err, code = run(ROOT, ["git", "ls-remote", repo_url, "HEAD"])
    if code != 0 or not out.strip():
        fail(
            f"could not resolve {config['repository']}'s default-branch HEAD via "
            f"`git ls-remote {repo_url} HEAD`:\n{err}"
        )
    sha = out.strip().split()[0]
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        fail(f'`git ls-remote {repo_url} HEAD` returned an unexpected line: "{out.strip()}"')
    return sha


target_commit = resolve_default_branch_head() if HEAD_MODE else config["commit"]
if HEAD_MODE:
    print(f"{LABEL}: HEAD mode — {config['repository']}'s current default-branch HEAD is {target_commit}")

cache_key = sha256(f"{target_commit}:{EXPECTED_DUCKDB_REF}".encode()).hexdigest()[:16]
cache_dir = CACHE_ROOT / cache_key
cached_artifact = cache_dir / "rawduck.duckdb_extension"

CHECKOUT_DIR.parent.mkdir(parents=True, exist_ok=True)


# 1. Ensure the checkout dir is at exactly target_commit (the pinned commit,
# or -- in HEAD mode -- the default-branch tip resolved above).
def current_head():
    out, _err, code = run(CHECKOUT_DIR, ["git", "rev-parse", "HEAD"])
    return out.strip() if code == 0 else None


if not (CHECKOUT_DIR / ".git").exists():
    print(f"{LABEL}: cloning {repo_url} into {CHECKOUT_DIR}")
    _out, err, code = run(ROOT, ["git", "clone", repo_url, str(CHECKOUT_DIR)])
    if code != 0:
        fail(f"could not clone {repo_url}:\n{err}")
else:
    head = current_head()
    if head is None:
        fail(f"{CHECKOUT_DIR} exists but is not a usable git checkout — remove it and re-run")

head = current_head()
if head != target_commit:
    print(f"{LABEL}: checking out {target_commit} (currently at {head})")
    _out, err, code = run(CHECKOUT_DIR, ["git", "checkout", "--detach", target_commit])
    if code != 0:
        # the commit may not be reachable from whatever refs were fetched at clone time
        _fout, _ferr, fcode = run(CHECKOUT_DIR, ["git", "fetch", "origin", target_commit])
        if fcode == 0:
            _out, err, code = run(CHECKOUT_DIR, ["git", "checkout", "--detach", target_commit])
    if code != 0:
        fail(
            (
                f"could not check out {config['repository']}'s own default-branch HEAD "
                f'"{target_commit}" — has it moved again since resolution?\n{err}'
            )
            if HEAD_MODE
            else (
                f'could not check out pinned commit "{target_commit}" in {config["repository"]} — '
                f'is scripts/partners/rawduck.json\'s "commit" field wrong?\n{err}'
            )
        )
    head = current_head()
    if head != target_commit:
        fail(f'checkout of "{target_commit}" reported success but HEAD is "{head}" — refusing to proceed')
checkout_desc = f"{config['repository']}'s current default-branch HEAD" if HEAD_MODE else "the pinned commit"
print(f"{LABEL}: {CHECKOUT_DIR} is at {checkout_desc} {target_commit}")

# 2. Cache hit: skip straight to placing the cached artifact and reporting.
if cached_artifact.exists():
    dest_dir = CHECKOUT_DIR / ARTIFACT_REL.parent
    dest_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(cached_artifact, CHECKOUT_DIR / ARTIFACT_REL)
    print(f"{LABEL}: PASS (cache hit, key {cache_key}) — HEAD {target_commit} — {CHECKOUT_DIR / ARTIFACT_REL}")
    sys.exit(0)

# 3. Submodules: extension-ci-tools stays at the partner's own pin (build
# tooling only); duckdb is re-pointed at our own pin regardless of what the
# partner's .gitmodules or gitlink says -- that re-pointing IS the point of
# this harness (layout parity requires the identical DuckDB version).
_out, err, code = run(CHECKOUT_DIR, ["git", "submodule", "update", "--init", "--", "extension-ci-tools"])
if code != 0:
    fail(f"could not init {config['repository']}'s own extension-ci-tools submodule:\n{err}")

_out, err, code = run(CHECKOUT_DIR, ["git", "submodule", "update", "--init", "--", "duckdb"])
if code != 0:
    fail(f"could not init {config['repository']}'s own duckdb submodule:\n{err}")
duckdb_dir = CHECKOUT_DIR / "duckdb"
_out, err, code = run(duckdb_dir, ["git", "checkout", EXPECTED_DUCKDB_REF])
if code != 0:
    _fout, ferr, fcode = run(duckdb_dir, ["git", "fetch", "origin", "tag", EXPECTED_DUCKDB_REF])
    if fcode == 0:
        _out, err, code = run(duckdb_dir, ["git", "checkout", EXPECTED_DUCKDB_REF])
if code != 0:
    fail(f'could not re-point {config["repository"]}\'s duckdb submodule at our own pin "{EXPECTED_DUCKDB_REF}":\n{err}')
print(f"{LABEL}: re-pointed {duckdb_dir} to our own pin {EXPECTED_DUCKDB_REF}")

# 4. Build, matching the root Makefile's own CMAKE_BUILD_PARALLEL_LEVEL convention.
nproc_out, _err, _code = run(ROOT, ["nproc"])
nproc = nproc_out.strip() or "1"
print(f"{LABEL}: building rawduck.duckdb_extension (commit {target_commit}, duckdb {EXPECTED_DUCKDB_REF})")
build_out, build_err, build_code = run(CHECKOUT_DIR, ["make", "release"], env={"CMAKE_BUILD_PARALLEL_LEVEL": nproc})
if build_code != 0:
    tail = "\n".join((build_out + build_err).split("\n")[-60:])
    commit_desc = f"{config['repository']}'s current default-branch HEAD commit" if HEAD_MODE else "partner commit"
    fail(
        f'rawduck failed to build at {commit_desc} "{target_commit}" against our duckdb pin '
        f'"{EXPECTED_DUCKDB_REF}" (exit {build_code}):\n{tail}'
    )

artifact_path = CHECKOUT_DIR / ARTIFACT_REL
if not artifact_path.exists():
    fail(f"build exited 0 but {ARTIFACT_REL} was not produced (HEAD {target_commit})")

# 5. Populate the cache for the next run with this same (commit, our pin).
cache_dir.mkdir(parents=True, exist_ok=True)
shutil.copyfile(artifact_path, cached_artifact)

print(f"{LABEL}: PASS (built, cached under key {cache_key}) — HEAD {target_commit} — {artifact_path}")
