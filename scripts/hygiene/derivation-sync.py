#!/usr/bin/env python3
# make derivation-sync
# #37's own sync test (DROPPED/UNDECLARED/UNRECORDED/VERSION-MISMATCH
# verdicts).
import json
import os
import sys

import yaml

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()

DERIVED_DIR = os.path.join(root, "test", "fixtures", "derived")
MANIFEST_PATH = os.path.join(DERIVED_DIR, "manifest.json")
ROSTER_PATH = os.path.join(root, "test", "fixtures", "roster.json")


def list_yaml_files(dir_):
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


def fail(violations):
    print("derivation-sync: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)


if not os.path.exists(MANIFEST_PATH):
    fail([f"{os.path.relpath(MANIFEST_PATH, root)} does not exist — nothing records what the derivation repository dropped here"])

try:
    with open(MANIFEST_PATH, "r", encoding="utf-8") as f:
        manifest = json.load(f)
except Exception as e:
    fail([f"{os.path.relpath(MANIFEST_PATH, root)}: could not parse JSON ({e})"])

structural = []
if not isinstance(manifest.get("tool"), str) or len(manifest.get("tool", "")) == 0:
    structural.append("manifest.tool must be a non-empty string")
if not isinstance(manifest.get("tool_version"), str) or len(manifest.get("tool_version", "")) == 0:
    structural.append("manifest.tool_version must be a non-empty string")
if not isinstance(manifest.get("fixtures"), list):
    structural.append("manifest.fixtures must be an array of fixture identities")
if structural:
    fail(structural)

expected_derived_by = f"{manifest['tool']}@{manifest['tool_version']}"
manifest_set = set(manifest["fixtures"])

violations = []
actual_ids = set()

for file in list_yaml_files(DERIVED_DIR):
    rel = os.path.relpath(file, root)
    try:
        with open(file, "r", encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
    except Exception as e:
        violations.append(f"{rel}: could not parse YAML ({e})")
        continue
    if not doc.get("fixture"):
        violations.append(f'{rel}: missing required "fixture" identity field')
        continue
    actual_ids.add(doc["fixture"])

    provenance = doc.get("provenance") or {}
    derived_by = provenance.get("derived_by") if isinstance(provenance, dict) else None
    if derived_by != expected_derived_by:
        violations.append(
            f'VERSION-MISMATCH: {rel} ("{doc["fixture"]}") has provenance.derived_by {json.dumps(derived_by)}, expected "{expected_derived_by}" (manifest.tool@manifest.tool_version)'
        )

for id_ in manifest_set:
    if id_ not in actual_ids:
        violations.append(f'DROPPED: manifest declares "{id_}" but no test/fixtures/derived/**/*.yaml fixture carries that identity')
for id_ in actual_ids:
    if id_ not in manifest_set:
        violations.append(f'UNDECLARED: "{id_}" exists under test/fixtures/derived but the manifest never declared it')

if os.path.exists(ROSTER_PATH):
    with open(ROSTER_PATH, "r", encoding="utf-8") as f:
        roster = json.load(f)
else:
    roster = []
roster_set = set(roster)
for id_ in manifest_set:
    if id_ not in roster_set:
        violations.append(f'UNRECORDED: "{id_}" is in the derivation manifest but not yet in {os.path.relpath(ROSTER_PATH, root)}')

if violations:
    fail(violations)
print(f"derivation-sync: PASS ({len(manifest_set)} manifest fixture(s), tool {expected_derived_by})")
