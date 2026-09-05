#!/usr/bin/env python3
"""make check-pins

Port of scripts/check-pins.mjs. Pin-consistency check (Article IV): the
duckdb and extension-ci-tools submodule pins must agree with each other on
the versions T0.3 fixes, and -- once
.github/workflows/MainDistributionPipeline.yml exists (T0.4) -- with the
duckdb_version/ci_tools_version it declares. Absent that file, the workflow
side is reported "pending", never a failure: this task doesn't own that
file.

Also reports (#47) the storage-partner pin declared in
scripts/partners/rawduck.json alongside the submodule pins above, and fails
when that pinned commit disagrees with what's actually checked out at
build/partners/rawduck/ -- but only when that directory exists. It's a
plain `git clone` (never a submodule, never committed -- see
scripts/partners/rawduck-build.mjs), so a hygiene-only run that never built
the partner has nothing to compare against; that absence is reported
"pending", exactly like the MainDistributionPipeline.yml case above and the
Makefile's own "extension-ci-tools submodule not checked out" warning
(never a failure -- the check that does build the partner first, the
partner-rawduck lane, is where this comparison actually bites).

Same shape again (#43) for the libchdb pin declared in
scripts/live-oracles/chdb.json: also never a submodule, never committed --
scripts/live-oracles/chdb-fetch.mjs fetches a checksummed release tarball
into build/live-oracles/chdb/<tag>/ instead of cloning a source tree. There
is no independent HEAD to compare against (there's no git checkout at
all); chdb-fetch.mjs itself already refuses to vendor anything whose
sha256 doesn't match the pin, so the fact that libchdb.so and chdb.h exist
at the pinned tag's own directory is the "actual matches pinned" evidence
here. Absent that directory, it's "pending" (run `make chdb-fetch`), same
as the rawduck case never being a failure on its own.
"""

import json
import platform
import re
import subprocess
import sys
from pathlib import Path

import yaml

from lib.duckdb_pin import EXPECTED_DUCKDB_REF

# The exact pins T0.3 fixes (CONSTITUTION.md Article IV / issue #16): duckdb at
# the tag v1.5.4, extension-ci-tools at the branch v1.5-variegata.
EXPECTED = {
    "duckdb": EXPECTED_DUCKDB_REF,
    "extension-ci-tools": "v1.5-variegata",
}


def _root_from_args(args):
    if "--root" in args:
        return Path(args[args.index("--root") + 1])
    return Path.cwd()


def _git(root, args):
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True)


def submodule_pin(root, name):
    result = _git(root, ["submodule", "status", "--", name])
    if result.returncode != 0:
        return {"sha": None, "ref": None, "error": f"could not read submodule status: {result.stderr.strip()}"}
    line = result.stdout.strip()
    if not line:
        return {"sha": None, "ref": None, "error": "not a registered submodule"}
    # Format: "[+-U]<sha> <path> (<describe>)" -- the describe suffix is what we
    # compare against. A branch checkout describes as "heads/<branch>" when a
    # local branch exists, or "remotes/<remote>/<branch>" on a fresh
    # `submodule update --init` (detached HEAD, no local branch -- only the
    # remote-tracking ref) -- both name the same branch, so strip either.
    # A tag checkout describes as the bare tag name, needing no stripping.
    m = re.match(r"^[ +\-U]?([0-9a-f]{40})\s+\S+(?:\s+\(([^)]+)\))?", line)
    if not m:
        return {"sha": None, "ref": None, "error": f'unparsable submodule status line: "{line}"'}
    sha, describe = m.group(1), m.group(2)
    ref = None
    if describe:
        ref = re.sub(r"^heads/", "", describe)
        ref = re.sub(r"^remotes/[^/]+/", "", ref)
    return {"sha": sha, "ref": ref, "error": None}


def workflow_versions(root):
    wf_path = root / ".github" / "workflows" / "MainDistributionPipeline.yml"
    if not wf_path.exists():
        return {"present": False, "duckdb": None, "ciTools": None, "error": None}

    try:
        doc = yaml.safe_load(wf_path.read_text(encoding="utf8"))
    except Exception as e:  # noqa: BLE001 - mirrors the .mjs's catch-all
        return {"present": True, "duckdb": None, "ciTools": None, "error": f"could not parse YAML ({e})"}

    jobs = list((doc or {}).get("jobs", {}).values())
    duckdb_versions = set()
    ci_tools_versions = set()
    for job in jobs:
        with_args = (job or {}).get("with") or {}
        if with_args.get("duckdb_version"):
            duckdb_versions.add(str(with_args["duckdb_version"]))
        if with_args.get("ci_tools_version"):
            ci_tools_versions.add(str(with_args["ci_tools_version"]))

    if len(duckdb_versions) > 1:
        return {
            "present": True,
            "duckdb": None,
            "ciTools": None,
            "error": f"jobs disagree on duckdb_version: {', '.join(sorted(duckdb_versions))}",
        }
    if len(ci_tools_versions) > 1:
        return {
            "present": True,
            "duckdb": None,
            "ciTools": None,
            "error": f"jobs disagree on ci_tools_version: {', '.join(sorted(ci_tools_versions))}",
        }

    return {
        "present": True,
        "duckdb": next(iter(duckdb_versions)) if duckdb_versions else None,
        "ciTools": next(iter(ci_tools_versions)) if ci_tools_versions else None,
        "error": None,
    }


# Partner pin (#47): scripts/partners/rawduck.json names the commit
# build/partners/rawduck/ (a plain clone, never a submodule) must actually be
# checked out at, once that clone exists.
def partner_pin_status(root):
    config_path = root / "scripts" / "partners" / "rawduck.json"
    if not config_path.exists():
        return {"present": False}
    try:
        config = json.loads(config_path.read_text(encoding="utf8"))
    except Exception as e:  # noqa: BLE001
        return {"present": True, "error": f"could not parse {config_path} ({e})"}
    checkout_path = root / "build" / "partners" / "rawduck"
    if not checkout_path.exists():
        return {"present": True, "pinned": config.get("commit"), "checkedOut": False}
    result = _git(checkout_path, ["rev-parse", "HEAD"])
    if result.returncode != 0:
        return {
            "present": True,
            "pinned": config.get("commit"),
            "checkedOut": True,
            "error": f"could not read HEAD of {checkout_path}: {result.stderr.strip()}",
        }
    return {"present": True, "pinned": config.get("commit"), "checkedOut": True, "actual": result.stdout.strip()}


# libchdb pin (#43): scripts/live-oracles/chdb.json names the pinned
# (repository, tag, per-platform sha256) scripts/live-oracles/chdb-fetch.mjs
# vendors into build/live-oracles/chdb/<tag>/ (never a submodule, never
# committed -- see that script's own header comment).
def libchdb_pin_status(root):
    config_path = root / "scripts" / "live-oracles" / "chdb.json"
    if not config_path.exists():
        return {"present": False}
    try:
        config = json.loads(config_path.read_text(encoding="utf8"))
    except Exception as e:  # noqa: BLE001
        return {"present": True, "error": f"could not parse {config_path} ({e})"}
    if not config.get("repository") or not config.get("tag"):
        return {"present": True, "error": f'{config_path} must declare "repository" and "tag"'}
    pinned = f"{config['repository']}@{config['tag']}"
    # Only linux-x86_64 is pinned today (chdb.json's own comment: the only
    # platform the chdb-differential CI lane runs on) -- the same
    # "no pin for this platform" case chdb-fetch.mjs itself fails loudly on,
    # reported here as a violation for the same reason, not silently skipped.
    is_linux_x64 = sys.platform.startswith("linux") and platform.machine() in ("x86_64", "amd64")
    platform_key = "linux-x86_64" if is_linux_x64 else None
    platform_pin = (config.get("platforms") or {}).get(platform_key) if platform_key else None
    if not platform_key or not platform_pin:
        return {
            "present": True,
            "pinned": pinned,
            "error": f"no pinned libchdb asset for this platform ({sys.platform}/{platform.machine()}) in {config_path}",
        }
    vendored_rel = Path("build") / "live-oracles" / "chdb" / config["tag"]
    vendored_dir = root / vendored_rel
    vendored = (vendored_dir / "libchdb.so").exists() and (vendored_dir / "chdb.h").exists()
    return {
        "present": True,
        "pinned": pinned,
        "sha256": platform_pin.get("sha256"),
        "vendoredRel": str(vendored_rel),
        "vendored": vendored,
        "error": None,
    }


def main():
    args = sys.argv[1:]
    root = _root_from_args(args)

    violations = []
    notes = []

    duckdb_pin = submodule_pin(root, "duckdb")
    ci_tools_pin = submodule_pin(root, "extension-ci-tools")

    if duckdb_pin["error"]:
        violations.append(f"duckdb submodule: {duckdb_pin['error']}")
    elif duckdb_pin["ref"] != EXPECTED["duckdb"]:
        violations.append(
            f'duckdb submodule pinned at "{duckdb_pin["ref"] or duckdb_pin["sha"]}", expected "{EXPECTED["duckdb"]}"'
        )
    else:
        notes.append(f"duckdb submodule pin: {duckdb_pin['ref']} ({duckdb_pin['sha']}) — OK")

    if ci_tools_pin["error"]:
        violations.append(f"extension-ci-tools submodule: {ci_tools_pin['error']}")
    elif ci_tools_pin["ref"] != EXPECTED["extension-ci-tools"]:
        violations.append(
            f'extension-ci-tools submodule pinned at "{ci_tools_pin["ref"] or ci_tools_pin["sha"]}", '
            f'expected "{EXPECTED["extension-ci-tools"]}"'
        )
    else:
        notes.append(f"extension-ci-tools submodule pin: {ci_tools_pin['ref']} ({ci_tools_pin['sha']}) — OK")

    wf = workflow_versions(root)
    if wf["error"]:
        violations.append(f".github/workflows/MainDistributionPipeline.yml: {wf['error']}")
    elif not wf["present"]:
        notes.append(
            "duckdb_version (workflow): pending — .github/workflows/MainDistributionPipeline.yml does not exist yet (T0.4)"
        )
        notes.append(
            "ci_tools_version (workflow): pending — .github/workflows/MainDistributionPipeline.yml does not exist yet (T0.4)"
        )
    else:
        if wf["duckdb"] != EXPECTED["duckdb"]:
            violations.append(f'workflow duckdb_version "{wf["duckdb"]}" does not match "{EXPECTED["duckdb"]}"')
        else:
            notes.append(f"workflow duckdb_version: {wf['duckdb']} — OK")
        if wf["ciTools"] != EXPECTED["extension-ci-tools"]:
            violations.append(
                f'workflow ci_tools_version "{wf["ciTools"]}" does not match "{EXPECTED["extension-ci-tools"]}"'
            )
        else:
            notes.append(f"workflow ci_tools_version: {wf['ciTools']} — OK")

    partner_pin = partner_pin_status(root)
    if partner_pin.get("present"):
        if partner_pin.get("error"):
            violations.append(f"rawduck partner pin: {partner_pin['error']}")
        elif not partner_pin.get("checkedOut"):
            notes.append(
                f"rawduck partner pin: {partner_pin['pinned']} — pending "
                "(build/partners/rawduck/ not checked out; run `make partner-rawduck-build`)"
            )
        elif partner_pin.get("actual") != partner_pin.get("pinned"):
            violations.append(
                f'rawduck partner pin "{partner_pin["pinned"]}" (scripts/partners/rawduck.json) does not match '
                f'the checkout at build/partners/rawduck/, which is at "{partner_pin["actual"]}"'
            )
        else:
            notes.append(f"rawduck partner pin: {partner_pin['pinned']} — OK (matches build/partners/rawduck/ checkout)")

    libchdb_pin = libchdb_pin_status(root)
    if libchdb_pin.get("present"):
        if libchdb_pin.get("error"):
            violations.append(f"libchdb pin: {libchdb_pin['error']}")
        elif not libchdb_pin.get("vendored"):
            notes.append(
                f"libchdb pin: {libchdb_pin['pinned']} — pending "
                f"({libchdb_pin['vendoredRel']} not vendored; run `make chdb-fetch`)"
            )
        else:
            notes.append(
                f"libchdb pin: {libchdb_pin['pinned']} — OK (vendored at {libchdb_pin['vendoredRel']}, "
                f"sha256 {libchdb_pin['sha256']} verified on fetch)"
            )

    for n in notes:
        print(n)

    if violations:
        print("check-pins: FAIL", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        sys.exit(1)
    print("check-pins: PASS")


if __name__ == "__main__":
    main()
