#!/usr/bin/env python3
# make chdb-differential (#43, T2.5) -- the L6a chDB leg of the SQL-substrate
# differential, on the merge gate (docs/testing/layers.md's L6a row: "[MERGE]
# (chDB)"). One driver, one back-end (chDB -- the Timescale leg is M3, out of
# this issue's scope per its own "Out of scope" note), one fixture format:
# every rate fixture (`test/fixtures/rate/*.yaml` and
# `test/fixtures/derived/**/*.yaml`) either runs through
# `test/live_oracles/chdb/chdb_diff_eval.cpp` (compiled once here, the same
# "compile once, drive over stdin" shape `scripts/hygiene/kernel-fixture-loader.py`
# already established for the L2 leg) or is recorded as a ✗-by-shape roster
# gap when chDB's own `timeSeriesRateToGrid(ts, value)` signature has no
# argument for a bound start timestamp (`docs/testing/live-oracles.md`: "A
# fixture an oracle cannot evaluate at all ... is not a divergence; it is a
# roster gap").
#
# Scope: the same "rate fixture corpus" definition
# `scripts/hygiene/kernel-fixture-loader.py` already established for the L2
# leg -- every fixture actually found under `test/fixtures/rate/*.yaml` OR
# `test/fixtures/derived/**/*.yaml`, read alongside each other rather than
# through a second, narrower scan. A fixture this leg cannot run through
# chDB's own signature is still a roster gap, exactly like the flat
# `test/fixtures/rate/` case; nothing about a fixture living under
# `derived/` changes that.
#
# Identity ratchet (Article V.4 / T7), generalised to three observed states
# rather than kernel-fixture-loader.py's two (pass/fail): a (fixture, oracle)
# pair is either a comparator PASS, a comparator FAIL, or a declared
# SHAPE_GAP. `test/fixtures/chdb-oracle-roster.json` records the "pass" set
# and the "shape_gap" set the current tree is known to produce; anything
# else is one of the four fatal verdicts:
#   - roster.pass entry, still PASS                    -> OK
#   - roster.pass entry, now FAIL or SHAPE_GAP          -> REGRESSED
#   - roster.pass entry, fixture no longer exists       -> VANISHED
#   - roster.shape_gap entry, still SHAPE_GAP            -> OK
#   - roster.shape_gap entry, now PASS (template gained  -> UNRECORDED
#     the capability -- an improvement the roster doesn't record)
#   - roster.shape_gap entry, now FAIL                   -> ARRIVED-FAILING
#   - roster.shape_gap entry, fixture no longer exists   -> VANISHED
#   - not in either roster set, now PASS or SHAPE_GAP    -> UNRECORDED
#   - not in either roster set, now FAIL                 -> ARRIVED-FAILING
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml

# chdb-fetch.py's hyphenated filename isn't a valid module name for a plain
# `import`, so load it by its exact file path instead of renaming the file
# the issue names -- the same file `make chdb-fetch` runs directly.
_chdb_fetch_spec = importlib.util.spec_from_file_location(
    "chdb_fetch", Path(__file__).resolve().parent / "chdb-fetch.py"
)
_chdb_fetch = importlib.util.module_from_spec(_chdb_fetch_spec)
_chdb_fetch_spec.loader.exec_module(_chdb_fetch)
ensureChdbVendored = _chdb_fetch.ensureChdbVendored

args = sys.argv[1:]
root_idx = args.index("--root") if "--root" in args else -1
root = Path(args[root_idx + 1]) if root_idx != -1 else Path.cwd()

FIXTURE_DIR = root / "test" / "fixtures" / "rate"
DERIVED_DIR = root / "test" / "fixtures" / "derived"
ROSTER_PATH = root / "test" / "fixtures" / "chdb-oracle-roster.json"
ORACLE = "chdb"

REAL_HERE = Path(__file__).resolve().parent
EVAL_CPP = REAL_HERE.parent.parent / "test" / "live_oracles" / "chdb" / "chdb_diff_eval.cpp"

# Which declared divergence (src/kernel/chdb_divergence.hpp) each fixture
# exercises, hand-mapped the same way `test/fixtures/rate/*.yaml`'s own
# `wrong:` sections name a specific bug rather than being inferred -- a
# fixture's relationship to a *named* divergence is exactly the kind of
# thing a scan should not guess at. Absent here means `NONE`: the fixture
# and chDB are expected to agree with nothing to name.
DIVERGENCE_BY_FIXTURE = {
    "rate/dup-duplicate-timestamp": "DUP_TS_KEEPS_MAX",
    "rate/threshold-single-sample-without-st": "NULL_FOR_TOO_FEW",
}


def fail(message):
    print(f"chdb-differential: FAIL — {message}", file=sys.stderr)
    sys.exit(1)


def run(cmd, input=None, env=None):
    full_env = {**os.environ, **env} if env else None
    proc = subprocess.run(
        cmd,
        input=input,
        capture_output=True,
        text=True,
        env=full_env,
    )
    return {"out": proc.stdout, "err": proc.stderr, "code": proc.returncode}


# Recursive, so `test/fixtures/derived/rate/*.yaml` (and any further
# per-source subdirectory a future batch adds) is found the same way
# `test/fixtures/rate/*.yaml`'s own flat listing already is -- one walker,
# mirroring `scripts/hygiene/kernel-fixture-loader.py`'s own, not a second
# copy for the nested case.
def list_fixture_files(directory):
    if not directory.exists():
        return []
    out = []

    def walk(d):
        for entry in sorted(os.listdir(d)):
            full = d / entry
            if full.is_dir():
                walk(full)
            elif entry.endswith(".yaml") or entry.endswith(".yml"):
                out.append(full)

    walk(directory)
    return out


# chDB's own `timeSeriesRateToGrid(ts, value)` has no argument for a bound
# start timestamp -- any 3-element `[t, v, st]` sample makes this fixture
# unrepresentable in chDB's signature at all (a roster gap, not a
# divergence).
def has_start_timestamp(doc):
    return any(len(s) >= 3 for s in doc["samples"])


def build_wire_payload(doc, divergence_tag):
    lines = []
    lines.append(f"GRID {doc['grid']['start']} {doc['grid']['end']} {doc['grid']['step']}")
    lines.append(f"WINDOW {doc['window']}")
    lines.append(f"NSAMPLES {len(doc['samples'])}")
    for sample in doc["samples"]:
        t, v = sample[0], sample[1]
        if not isinstance(v, (int, float)) or isinstance(v, bool):
            raise ValueError(
                f"sample value {json.dumps(v)} is not a plain number (NaN/stale samples are out of this leg's scope)"
            )
        lines.append(f"{t} {v}")
    lines.append(f"DIVERGENCE {divergence_tag if divergence_tag is not None else 'NONE'}")
    return "\n".join(lines) + "\n"


# #229/#235: chdb_diff_eval.cpp reaches test/kernel/rate_fixture_eval.hpp via
# "../../kernel/rate_fixture_eval.hpp", which in turn reaches its own kernel
# headers with the clean, non-`../` `#include "kernel/foo.hpp"` form that
# #229 gave every test/kernel/*.cpp file. That form only resolves against
# `<root>/src` (the same root `kernel-primitive-tests.py` and
# `kernel-fixture-loader.py` use), so this g++ invocation needs the
# matching `-I <root>/src` flag even though chdb_diff_eval.cpp itself lives
# outside test/kernel/ and keeps its own original relative includes.
def compile_evaluator(tmp_dir, chdb_dir):
    bin_path = tmp_dir / "chdb_diff_eval"
    compile_result = run(
        [
            "g++",
            "-std=c++17",
            "-O1",
            "-Wall",
            "-Wextra",
            f"-I{root / 'src'}",
            f"-I{chdb_dir}",
            str(EVAL_CPP),
            "-o",
            str(bin_path),
            f"-L{chdb_dir}",
            "-lchdb",
            f"-Wl,-rpath,{chdb_dir}",
        ]
    )
    if compile_result["code"] != 0:
        raise RuntimeError(
            f"failed to compile {os.path.relpath(EVAL_CPP, root)}:\n{compile_result['out']}{compile_result['err']}"
        )
    return bin_path


def main():
    vendored = ensureChdbVendored()
    chdb_dir = vendored["dir"]
    tmp = Path(tempfile.mkdtemp(prefix="chdb-differential-"))
    bin_path = compile_evaluator(tmp, chdb_dir)

    files = list_fixture_files(FIXTURE_DIR) + list_fixture_files(DERIVED_DIR)
    current = {}  # "<fixture>@chdb" -> { state, rel, diag }

    for file in files:
        rel = os.path.relpath(file, root)
        doc = yaml.safe_load(file.read_text()) or {}
        if not doc.get("fixture"):
            fail(f'{rel}: missing required "fixture" identity field')
        if doc.get("function") != "rate":
            continue  # this leg only carries the rate family today

        key = f"{doc['fixture']}@{ORACLE}"

        if has_start_timestamp(doc):
            current[key] = {
                "state": "shape_gap",
                "rel": rel,
                "diag": "chDB's timeSeriesRateToGrid has no start-timestamp argument",
            }
            continue

        payload = build_wire_payload(doc, DIVERGENCE_BY_FIXTURE.get(doc["fixture"]))
        result = run([str(bin_path)], input=payload, env={"LD_LIBRARY_PATH": str(chdb_dir)})
        diag = result["out"] + result["err"]
        if re.search(r"^RESULT PASS$", result["out"], re.MULTILINE):
            current[key] = {"state": "pass", "rel": rel, "diag": diag}
        else:
            current[key] = {"state": "fail", "rel": rel, "diag": diag}

    roster = json.loads(ROSTER_PATH.read_text()) if ROSTER_PATH.exists() else {"pass": [], "shape_gap": []}
    # dict.fromkeys(...), not set(...): an insertion-ordered de-duplicated
    # container, so the roster's own declared order is preserved. A plain
    # Python set's iteration order is hash-based, not insertion-based, which
    # would make the violation ordering below vary run to run whenever more
    # than one violation lands in the same category.
    roster_pass = dict.fromkeys(roster.get("pass") or [])
    roster_gap = dict.fromkeys(roster.get("shape_gap") or [])

    violations = []

    for id_ in roster_pass:
        entry = current.get(id_)
        if not entry:
            violations.append(f'VANISHED: "{id_}" is in the roster\'s pass set but no current fixture declares that identity')
        elif entry["state"] != "pass":
            violations.append(
                f'REGRESSED: "{id_}" ({entry["rel"]}) was PASS but is now {entry["state"].upper()}:\n{entry["diag"]}'
            )
    for id_ in roster_gap:
        entry = current.get(id_)
        if not entry:
            violations.append(f'VANISHED: "{id_}" is in the roster\'s shape_gap set but no current fixture declares that identity')
        elif entry["state"] == "fail":
            violations.append(f'ARRIVED-FAILING: "{id_}" ({entry["rel"]}) is a declared shape gap but now fails outright:\n{entry["diag"]}')
        elif entry["state"] == "pass":
            violations.append(f'UNRECORDED: "{id_}" ({entry["rel"]}) is a declared shape gap but now passes — update the roster')
    for id_, entry in current.items():
        if id_ in roster_pass or id_ in roster_gap:
            continue
        if entry["state"] == "fail":
            violations.append(f'ARRIVED-FAILING: "{id_}" ({entry["rel"]}) is new and fails:\n{entry["diag"]}')
        else:
            state_desc = "passes" if entry["state"] == "pass" else "is a shape gap"
            violations.append(f'UNRECORDED: "{id_}" ({entry["rel"]}) is new and {state_desc} — not yet in {os.path.relpath(ROSTER_PATH, root)}')

    if violations:
        print("chdb-differential: FAIL", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        sys.exit(1)

    n_pass = sum(1 for e in current.values() if e["state"] == "pass")
    n_gap = sum(1 for e in current.values() if e["state"] == "shape_gap")
    print(f"chdb-differential: PASS ({n_pass} fixture(s) compared against chDB, {n_gap} declared shape gap(s))")


if __name__ == "__main__":
    main()
