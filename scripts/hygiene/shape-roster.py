#!/usr/bin/env python3
# shape-roster — T7 (docs/testing/rules.md: "Ratchets gate on identity,
# never counts... Every roster — fixture IDs, ShapeIDs, partition schemes,
# mutation legs — records the set that must pass") applied to the L3
# property roster `docs/testing/registry-and-fixtures.md` names: "the
# property roster (each name x edge mode x value domain is a ShapeID)".
#
# See scripts/hygiene/shape-roster.mjs's own header comment for the full
# rationale (verdict shapes, citation convention, roster derivation) — this
# is a mechanical port, unchanged.
import json
import os
import re
import sys

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()

REGISTRY_DEF_PATH = os.path.join("src", "kernel", "registry.def")
SHAPE_EXAMPLES_PATH = os.path.join(root, "test", "oracle", "shape_examples.hpp")
ROSTER_PATH = os.path.join(root, "test", "oracle", "shape-roster.json")

TS_FN_ROW_RE = re.compile(
    r"^\s*TS_FN\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_|][A-Za-z0-9_|]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)",
    re.MULTILINE,
)


def parse_registry_shape_ids(root):
    def_path = os.path.join(root, REGISTRY_DEF_PATH)
    if not os.path.exists(def_path):
        return None
    with open(def_path, "r", encoding="utf-8") as f:
        content = f.read()
    ids = set()
    for m in TS_FN_ROW_RE.finditer(content):
        name, _, _, _, edge_modes, domain, _ = m.groups()
        if edge_modes == "EDGE_NONE" or domain == "DOMAIN_NONE":
            continue
        bare_name = re.sub(r"^ts_", "", name)
        for mode in edge_modes.split("|"):
            ids.add(f"{bare_name}/{mode}/{domain}")
    return ids


SHAPE_CITATION_RE = re.compile(r"^// ShapeID: ([A-Za-z_][A-Za-z0-9_]*/[A-Z_]+/[A-Z_]+)\s*$", re.MULTILINE)


def parse_cited_shape_ids():
    if not os.path.exists(SHAPE_EXAMPLES_PATH):
        return set()
    with open(SHAPE_EXAMPLES_PATH, "r", encoding="utf-8") as f:
        content = f.read()
    return set(m.group(1) for m in SHAPE_CITATION_RE.finditer(content))


registry_shapes = parse_registry_shape_ids(root)
if registry_shapes is None:
    print("shape-roster: FAIL", file=sys.stderr)
    print(f"  {REGISTRY_DEF_PATH} does not exist", file=sys.stderr)
    sys.exit(1)

cited_shapes = parse_cited_shape_ids()
if os.path.exists(ROSTER_PATH):
    with open(ROSTER_PATH, "r", encoding="utf-8") as f:
        roster = json.load(f)
else:
    roster = []
roster_set = set(roster)

violations = []

for id_ in roster_set:
    if id_ not in registry_shapes:
        violations.append(f'VANISHED: "{id_}" is in the roster but src/kernel/registry.def no longer declares that shape')
    elif id_ not in cited_shapes:
        violations.append(
            f'REGRESSED: "{id_}" is in the roster and still a real registry shape, but no "// ShapeID: {id_}" citation exists in test/oracle/shape_examples.hpp any more'
        )

for id_ in registry_shapes:
    if id_ in roster_set:
        continue
    if id_ in cited_shapes:
        violations.append(f'UNRECORDED: "{id_}" has a worked example (test/oracle/shape_examples.hpp) but is not yet in test/oracle/shape-roster.json')
    else:
        violations.append(f'ARRIVED-FAILING: "{id_}" is a registered (name, edge_mode, domain) shape with no worked example at all in test/oracle/shape_examples.hpp')

for id_ in cited_shapes:
    if id_ not in registry_shapes:
        violations.append(f'UNKNOWN-SHAPE: test/oracle/shape_examples.hpp cites "{id_}", which is not a (name, edge_mode, domain) combination src/kernel/registry.def currently declares')

if violations:
    print("shape-roster: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print(f"shape-roster: PASS ({len(roster_set)} shape(s) in the roster)")
