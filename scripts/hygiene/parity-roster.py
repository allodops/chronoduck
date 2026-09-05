#!/usr/bin/env python3
# parity-roster — issue #44 (T2.6)'s own closure leg: L8's "operator ≡
# aggregate form" parity, registry-driven and identity-ratcheted the same
# way shape-roster.py already ratchets the L3 ShapeID roster (T7,
# docs/testing/rules.md: "Every roster — fixture IDs, ShapeIDs, partition
# schemes, mutation legs — records the set that must pass").
#
# The registry side: every src/kernel/registry.def row whose family is
# RANGE or HIST and whose det is D0 (docs/testing/layers.md's L8 row: "for
# every RANGE/HIST row — the operator, bit-exact for D0") must have a
# parity leg. The citation side: a `# L8-PARITY: <name>` comment naming the
# row, anywhere under test/sql/ — the marker this issue's own PR places
# next to the bit-exact operator/aggregate roster it names (the EXCEPT-both-
# ways views plus the threads x preserve_insertion_order sweep). This scan
# only proves the marker (and therefore the leg it names) exists; that the
# leg actually asserts bit-exact agreement is a review-time reading of the
# cited file, the same posture shape-roster.py takes for its own citations.
#
# Adding a RANGE row to registry.def with no matching citation anywhere
# fails as ARRIVED-FAILING, naming the row — issue #44's own acceptance
# criterion ("Adding a RANGE row without the parity leg is red").
import os
import re
import sys

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()

REGISTRY_DEF_PATH = os.path.join("src", "kernel", "registry.def")
TEST_SQL_DIR = os.path.join(root, "test", "sql")

TS_FN_ROW_RE = re.compile(
    r"^\s*TS_FN\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_|][A-Za-z0-9_|]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)",
    re.MULTILINE,
)

# The rows this leg actually applies to: RANGE/HIST family, D0 determinism —
# docs/testing/layers.md's own L8 scoping, not every registry row.
PARITY_FAMILIES = {"RANGE", "HIST"}


def parse_parity_rows(root):
    def_path = os.path.join(root, REGISTRY_DEF_PATH)
    if not os.path.exists(def_path):
        return None
    with open(def_path, "r", encoding="utf-8") as f:
        content = f.read()
    names = set()
    for m in TS_FN_ROW_RE.finditer(content):
        name, family, _state, det, _edge_modes, _domain, _scale_kind = m.groups()
        if family in PARITY_FAMILIES and det == "D0":
            names.add(name)
    return names


def list_files_under(dir_):
    files = []
    if not os.path.isdir(dir_):
        return files

    def walk(d):
        for entry in sorted(os.listdir(d)):
            full = os.path.join(d, entry)
            if os.path.isdir(full):
                walk(full)
            else:
                files.append(full)

    walk(dir_)
    return files


PARITY_CITATION_RE = re.compile(r"^#\s*L8-PARITY:\s*([A-Za-z_][A-Za-z0-9_]*)\s*$", re.MULTILINE)


def parse_cited_rows():
    cited = set()
    for path in list_files_under(TEST_SQL_DIR):
        if not path.endswith(".test"):
            continue
        try:
            with open(path, "r", encoding="utf-8") as f:
                content = f.read()
        except OSError:
            continue
        for m in PARITY_CITATION_RE.finditer(content):
            cited.add(m.group(1))
    return cited


ROSTER_PATH = os.path.join(root, "test", "sql", "parity-roster.json")

registry_rows = parse_parity_rows(root)
if registry_rows is None:
    print("parity-roster: FAIL", file=sys.stderr)
    print(f"  {REGISTRY_DEF_PATH} does not exist", file=sys.stderr)
    sys.exit(1)

cited_rows = parse_cited_rows()

if os.path.exists(ROSTER_PATH):
    import json

    with open(ROSTER_PATH, "r", encoding="utf-8") as f:
        roster = json.load(f)
else:
    roster = []
roster_set = set(roster)

violations = []

for name in roster_set:
    if name not in registry_rows:
        violations.append(f'VANISHED: "{name}" is in the roster but src/kernel/registry.def no longer declares it as a D0 RANGE/HIST row')
    elif name not in cited_rows:
        violations.append(f'REGRESSED: "{name}" is in the roster and still a real D0 RANGE/HIST row, but no "# L8-PARITY: {name}" citation exists anywhere under test/sql/ any more')

for name in registry_rows:
    if name in roster_set:
        continue
    if name in cited_rows:
        violations.append(f'UNRECORDED: "{name}" has a "# L8-PARITY: {name}" citation but is not yet in test/sql/parity-roster.json')
    else:
        violations.append(f'ARRIVED-FAILING: "{name}" is a D0 RANGE/HIST registry row with no "# L8-PARITY: {name}" citation anywhere under test/sql/')

for name in cited_rows:
    if name not in registry_rows:
        violations.append(f'UNKNOWN-ROW: a "# L8-PARITY: {name}" citation exists under test/sql/, but "{name}" is not a D0 RANGE/HIST row src/kernel/registry.def currently declares')

if violations:
    print("parity-roster: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print(f"parity-roster: PASS ({len(roster_set)} row(s) in the roster)")
