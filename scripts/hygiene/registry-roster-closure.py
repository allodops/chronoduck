#!/usr/bin/env python3
# make registry-roster-closure — issue #36's second meta-test (which
# rosters this fences, the DOMAIN_NONE carve-out, the ts_ prefix stripping
# convention).
import os
import re
import sys

import yaml

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()

REGISTRY_DEF_PATH = os.path.join("src", "kernel", "registry.def")
FIXTURES_DIR = os.path.join(root, "test", "fixtures")

TS_FN_ROW_RE = re.compile(
    r"^\s*TS_FN\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)",
    re.MULTILINE,
)


def parse_registry_rows(root):
    def_path = os.path.join(root, REGISTRY_DEF_PATH)
    if not os.path.exists(def_path):
        return None
    with open(def_path, "r", encoding="utf-8") as f:
        content = f.read()
    rows = []
    for m in TS_FN_ROW_RE.finditer(content):
        name, family, state, det, edge_modes, domain, scale_kind = m.groups()
        rows.append({"name": name, "family": family, "state": state, "det": det, "edgeModes": edge_modes, "domain": domain, "scaleKind": scale_kind})
    return rows


def list_yaml_files(dir_):
    if not os.path.isdir(dir_):
        return []
    out = []

    def walk(d):
        for entry in os.listdir(d):
            full = os.path.join(d, entry)
            if os.path.isdir(full):
                walk(full)
            elif entry.endswith(".yaml") or entry.endswith(".yml"):
                out.append(full)

    walk(dir_)
    return out


rows = parse_registry_rows(root)
if rows is None:
    print("registry-roster-closure: FAIL", file=sys.stderr)
    print(f"  {REGISTRY_DEF_PATH} does not exist", file=sys.stderr)
    sys.exit(1)

fixture_functions = set()
for file in list_yaml_files(FIXTURES_DIR):
    try:
        with open(file, "r", encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
    except Exception:
        continue
    if isinstance(doc, dict) and isinstance(doc.get("function"), str):
        fixture_functions.add(doc["function"])

violations = []
for row in rows:
    if row["domain"] == "DOMAIN_NONE":
        continue
    bare_name = re.sub(r"^ts_", "", row["name"])
    if bare_name not in fixture_functions:
        violations.append(
            f'row "{row["name"]}" declares domain={row["domain"]} (fixture-representable) but no test/fixtures/**/*.yaml fixture has function: {bare_name}'
        )

if violations:
    print("registry-roster-closure: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print(f"registry-roster-closure: PASS ({len(rows)} row(s), {len(fixture_functions)} distinct fixture function(s))")
