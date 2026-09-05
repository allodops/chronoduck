#!/usr/bin/env python3
# make kernel-fixture-loader
# The L2 fixture-replay harness this issue's (#33) Goal names: "Loader runs
# every fixture with the comparator; roster with
# REGRESSED/VANISHED/ARRIVED-FAILING/UNRECORDED" (docs/testing/layers.md's
# L2 row, docs/testing/rules.md's T7). See kernel-fixture-loader.mjs's own
# header comment for the full rationale — this is a mechanical port,
# unchanged.
import os
import re
import subprocess
import sys
import tempfile

import yaml

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()

FIXTURE_DIR = os.path.join(root, "test", "fixtures", "rate")
DERIVED_DIR = os.path.join(root, "test", "fixtures", "derived")
ROSTER_PATH = os.path.join(root, "test", "fixtures", "roster.json")

# The evaluator's own C++ source is this script's machinery, not part of
# the tree under --root — always compiled from this script's real location,
# so a materialized selftest root (which never includes a copy of it) still
# compiles the one real evaluator.
REAL_HERE = os.path.dirname(os.path.abspath(__file__))
REAL_ROOT = os.path.join(REAL_HERE, "..", "..")
LOADER_CPP = os.path.join(REAL_ROOT, "test", "kernel", "rate_fixture_loader.cpp")

SUPPORTED_EDGE_MODE = "EXTRAPOLATE"
SUPPORTED_DOMAIN = "COUNTER"


def run(cmd, input_=None):
    proc = subprocess.run(cmd, capture_output=True, text=True, input=input_)
    return proc.stdout, proc.stderr, proc.returncode


def list_fixture_files(dir_):
    if not os.path.isdir(dir_):
        return []
    out = []

    def walk(d):
        for entry in sorted(os.listdir(d)):
            full = os.path.join(d, entry)
            if os.path.isdir(full):
                walk(full)
            elif entry.endswith(".yaml") or entry.endswith(".yml"):
                out.append(full)

    walk(dir_)
    return out


def sample_line(sample, rel):
    t, v = sample[0], sample[1]
    st = sample[2] if len(sample) > 2 else None
    if not isinstance(v, (int, float)) or isinstance(v, bool):
        raise ValueError(f"{rel}: sample value {json_repr(v)} is not a plain number (NaN/stale samples are out of this loader's scope)")
    has_st = st is not None
    return f"{t} {v} {1 if has_st else 0} {st if has_st else 0}"


def json_repr(v):
    import json

    return json.dumps(v)


def expected_line(value):
    return "NULL" if value is None else str(value)


def build_wire_payload(doc, rel):
    lines = [f"GRID {doc['grid']['start']} {doc['grid']['end']} {doc['grid']['step']}", f"WINDOW {doc['window']}", f"NSAMPLES {len(doc['samples'])}"]
    for s in doc["samples"]:
        lines.append(sample_line(s, rel))
    lines.append(f"NEXPECTED {len(doc['expected'])}")
    for e in doc["expected"]:
        lines.append(expected_line(e))
    return "\n".join(lines) + "\n"


def compile_loader(tmp_dir):
    bin_path = os.path.join(tmp_dir, "rate_fixture_loader")
    out, err, code = run(["g++", "-std=c++17", "-Wall", "-Wextra", "-I", os.path.join(REAL_ROOT, "src"), LOADER_CPP, "-o", bin_path])
    if code != 0:
        rel = os.path.relpath(LOADER_CPP, root)
        raise RuntimeError(f"failed to compile {rel}:\n{out}{err}")
    return bin_path


def evaluate_fixture(bin_path, doc, rel):
    if doc.get("function") != "rate":
        return None
    if doc.get("edge_mode") != SUPPORTED_EDGE_MODE or doc.get("domain") != SUPPORTED_DOMAIN:
        raise RuntimeError(
            f"{rel}: edge_mode={doc.get('edge_mode')}/domain={doc.get('domain')} is outside this loader's scope ({SUPPORTED_EDGE_MODE}/{SUPPORTED_DOMAIN} only)"
        )
    payload = build_wire_payload(doc, rel)
    out, err, code = run([bin_path], input_=payload)
    passed = code == 0 and re.search(r"^RESULT PASS$", out, re.MULTILINE) is not None
    return {"pass": passed, "out": out, "err": err}


def main():
    tmp = tempfile.mkdtemp(prefix="kernel-fixture-loader-")
    bin_path = compile_loader(tmp)

    files = list_fixture_files(FIXTURE_DIR) + list_fixture_files(DERIVED_DIR)
    current = {}

    for file in files:
        rel = os.path.relpath(file, root)
        with open(file, "r", encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        if not doc.get("fixture"):
            print("kernel-fixture-loader: FAIL", file=sys.stderr)
            print(f'  {rel}: missing required "fixture" identity field', file=sys.stderr)
            sys.exit(1)
        outcome = evaluate_fixture(bin_path, doc, rel)
        if outcome is None:
            continue
        current[doc["fixture"]] = {"pass": outcome["pass"], "rel": rel, "diag": outcome["out"] + outcome["err"]}

    if os.path.exists(ROSTER_PATH):
        import json

        with open(ROSTER_PATH, "r", encoding="utf-8") as f:
            roster = json.load(f)
    else:
        roster = []
    roster_set = set(roster)

    violations = []
    for id_ in roster_set:
        entry = current.get(id_)
        if not entry:
            violations.append(f'VANISHED: "{id_}" is in the roster but no current fixture declares that identity')
        elif not entry["pass"]:
            violations.append(f'REGRESSED: "{id_}" ({entry["rel"]}) is in the roster but fails now:\n{entry["diag"]}')
    for id_, entry in current.items():
        if id_ in roster_set:
            continue
        if entry["pass"]:
            violations.append(f'UNRECORDED: "{id_}" ({entry["rel"]}) passes but is not yet in {os.path.relpath(ROSTER_PATH, root)}')
        else:
            violations.append(f'ARRIVED-FAILING: "{id_}" ({entry["rel"]}) is new and fails:\n{entry["diag"]}')

    if violations:
        print("kernel-fixture-loader: FAIL", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        sys.exit(1)
    print(f"kernel-fixture-loader: PASS ({len(current)} rate fixture(s), roster of {len(roster_set)})")


main()
