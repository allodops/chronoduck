#!/usr/bin/env python3
# make partner-rawduck-test (L15, issue #47)
#
# Loads chronoduck's own built extension AND the freshly built rawduck
# extension into the same stock DuckDB shell -- build/release/duckdb, exactly
# the binary scripts/smoke.mjs already treats as "a stock DuckDB shell" for
# chronoduck's own LOAD, -unsigned since neither .duckdb_extension is signed
# -- and runs every test/partners/rawduck/*.sql file against it.
#
# Smoke-LOAD only, per this issue's own scope (T2.9): confirms both
# extensions load together without conflict and a minimal RawDuck table
# answers a ts_rate query. Real fixture-driven layout-parity testing against
# RawDuck's on-disk layout is issue #48's job, not this script's.
#
# A *.sql file here is a plain SQL script, not a DuckDB sqllogictest file --
# deliberately not named `*.test`: DuckDB's own sqllogictest runner
# (`./build/release/test/unittest "test/*"`, what `make test` invokes)
# auto-discovers every `.test` file under `test/` regardless of directory
# and tries to parse it as sqllogictest syntax; a plain SQL script's `--`
# comment header isn't valid sqllogictest, so a `.test` extension here broke
# the ordinary test lane, not just this partner-specific one. Separately,
# sqllogictest's `require <extension>` directive resolves against DuckDB's
# own known-extension list, which a partner extension built out-of-tree
# under build/partners/rawduck/ is not part of, so the real DuckDB unittest
# runner could never load this file's contents even if the extension were
# `.test`. Executed the same way scripts/smoke.mjs executes its own
# one-liner: as a single `-c` script string against the CLI, success meaning
# exit 0 and no "Error:"-prefixed line in the output.
#
# HEAD mode (`partner-rawduck-head`, L15, issue #49): with RAWDUCK_REF=head
# in the environment, this targets the build produced by
# `RAWDUCK_REF=head make partner-rawduck-build` at
# build/partners/rawduck-head/ instead of the pinned checkout at
# build/partners/rawduck/, and reports the actual commit under test (the
# partner's default-branch HEAD at build time) alongside any failing leg.
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HEAD_MODE = os.environ.get("RAWDUCK_REF") == "head"
LABEL = "partner-rawduck-head-test" if HEAD_MODE else "partner-rawduck-test"

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
DUCKDB_BIN = ROOT / "build" / "release" / "duckdb"
CHRONODUCK_EXT = ROOT / "build" / "release" / "extension" / "chronoduck" / "chronoduck.duckdb_extension"
RAWDUCK_ROOT = ROOT / "build" / "partners" / ("rawduck-head" if HEAD_MODE else "rawduck")
RAWDUCK_EXT = RAWDUCK_ROOT / "build" / "release" / "extension" / "rawduck" / "rawduck.duckdb_extension"
# RawDuck's own extension_config.cmake also builds DuckDB's core `json`
# extension as a sibling artifact (RawDuck's ingest path depends on the JSON
# logical type / to_json() being registered -- see its README's "JSON
# extension: provides the JSON logical type and to_json()/json_* functions
# that RawDuck relies on for its structural-conflict columns"). LOADing it
# explicitly from the sibling artifact keeps this test hermetic -- no
# reliance on DuckDB's online-autoinstall reaching the network in CI.
RAWDUCK_JSON_EXT = RAWDUCK_ROOT / "build" / "release" / "extension" / "json" / "json.duckdb_extension"
TEST_DIR = ROOT / "test" / "partners" / "rawduck"


def fail(message):
    print(f"{LABEL}: FAIL — {message}", file=sys.stderr)
    sys.exit(1)


build_hint = "RAWDUCK_REF=head make partner-rawduck-build" if HEAD_MODE else "make partner-rawduck-build"

if not DUCKDB_BIN.exists():
    fail(f"{DUCKDB_BIN} does not exist — run `make release` first")
if not CHRONODUCK_EXT.exists():
    fail(f"{CHRONODUCK_EXT} does not exist — run `make release` first")
if not RAWDUCK_EXT.exists():
    fail(f"{RAWDUCK_EXT} does not exist — run `{build_hint}` first")
if not TEST_DIR.exists():
    fail(f"{TEST_DIR} does not exist")

# HEAD mode: name the commit under test up front -- the checkout at
# RAWDUCK_ROOT was left at whatever commit the paired build step resolved
# and built (this script never re-resolves it, so the reported commit is
# exactly what was actually built and is about to be tested).
if HEAD_MODE:
    head_rev = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=str(RAWDUCK_ROOT), capture_output=True, text=True
    )
    head_sha = head_rev.stdout.strip() if head_rev.returncode == 0 else None
    if not head_sha:
        fail(f"could not read HEAD of {RAWDUCK_ROOT} — run `{build_hint}` first")
    print(f"{LABEL}: testing RawDuck at HEAD commit {head_sha}")

test_files = sorted(f.name for f in TEST_DIR.iterdir() if f.name.endswith(".sql"))
if not test_files:
    fail("no test/partners/rawduck/*.sql files found")

preamble = "\n".join(
    line
    for line in [
        f"LOAD '{RAWDUCK_JSON_EXT}';" if RAWDUCK_JSON_EXT.exists() else None,
        f"LOAD '{CHRONODUCK_EXT}';",
        f"LOAD '{RAWDUCK_EXT}';",
    ]
    if line is not None
)

failures = 0
for file in test_files:
    script = (TEST_DIR / file).read_text()
    # A fresh scratch cwd per file: RawDuck's `ATTACH 'rawduck:<relative path>'`
    # creates a real on-disk store, which must never collide across test files
    # or across repeat runs of this same file.
    cwd = tempfile.mkdtemp(prefix="partner-rawduck-test-")
    proc = subprocess.run(
        [str(DUCKDB_BIN), "-unsigned", "-c", f"{preamble}\n{script}"],
        cwd=cwd,
        capture_output=True,
        text=True,
    )
    combined = proc.stdout + proc.stderr
    erred = proc.returncode != 0 or re.search(r"^Error:", combined, re.MULTILINE)
    if erred:
        print(f"{LABEL}: FAIL — test/partners/rawduck/{file}", file=sys.stderr)
        print(combined.strip(), file=sys.stderr)
        failures += 1
    else:
        print(f"{LABEL}: PASS — test/partners/rawduck/{file}")

if failures > 0:
    print(f"{LABEL}: FAIL ({failures}/{len(test_files)} file(s))", file=sys.stderr)
    sys.exit(1)
print(f"{LABEL}: PASS ({len(test_files)} file(s))")
