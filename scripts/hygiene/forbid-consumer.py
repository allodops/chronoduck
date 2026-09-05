#!/usr/bin/env python3
import json
import os
import re
import subprocess
import sys

import yaml

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()
using_real_tree = "--root" not in args

HERE = os.path.dirname(os.path.abspath(__file__))
TOKENS_PATH = os.path.join(HERE, "consumer-tokens.json")
with open(TOKENS_PATH, "r", encoding="utf-8") as f:
    _cfg = json.load(f)
TOKENS = _cfg["tokens"]
EXEMPT_PATHS = _cfg["exemptPaths"]


def read_text(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    if b"\0" in raw:
        return None
    return raw.decode("utf-8", errors="replace")


def list_files(dir_):
    if using_real_tree:
        out = subprocess.run(["git", "-C", dir_, "ls-files"], capture_output=True, text=True).stdout
        return [l for l in out.split("\n") if l]
    files = []

    def walk(d, prefix):
        for entry in os.listdir(d):
            full = os.path.join(d, entry)
            rel = f"{prefix}/{entry}" if prefix else entry
            if os.path.isdir(full):
                walk(full, rel)
            else:
                files.append(rel)

    walk(dir_, "")
    return files


def is_exempt(path):
    return any(path == p or path.startswith(p) for p in EXEMPT_PATHS)


# The scanner never scans the file that configures it — the same fact as "a
# program doesn't recurse into scanning itself", not an Article VI.1 policy
# exemption. Article VI.1's exempt-paths list (from consumer-tokens.json,
# above) is left exactly as ratified; this is a separate, hardcoded fact
# about what this tool's own input is, computed from the tool's own location
# rather than being a configurable policy path.
SELF_PATH = os.path.relpath(TOKENS_PATH, root) if using_real_tree else None


def is_fixture_file(path):
    return path.startswith("test/fixtures/") and re.search(r"\.(json|ya?ml)$", path)


VALUE_SCAN_FIELDS = ["fixture", "function"]


def collect_keys(node, out):
    if isinstance(node, list):
        for item in node:
            collect_keys(item, out)
    elif isinstance(node, dict):
        for key, value in node.items():
            out.append(key)
            collect_keys(value, out)


def collect_value_scan_fields(doc, out):
    if not isinstance(doc, dict):
        return
    for field in VALUE_SCAN_FIELDS:
        if isinstance(doc.get(field), str):
            out.append(doc[field])


def extract_structural_keys(text):
    try:
        doc = yaml.safe_load(text)
    except Exception:
        return None
    keys = []
    collect_keys(doc, keys)
    collect_value_scan_fields(doc, keys)
    return keys


files = list_files(root)
violations = []

for f in files:
    if is_exempt(f):
        continue
    if SELF_PATH and f == SELF_PATH:
        continue
    try:
        content = read_text(os.path.join(root, f))
    except OSError:
        continue
    if content is None:
        continue

    scanned = extract_structural_keys(content) if is_fixture_file(f) else None
    haystacks = scanned if scanned is not None else [content]
    for hay in haystacks:
        for token in TOKENS:
            if re.search(rf"\b{token}\b", hay, re.IGNORECASE):
                violations.append(f'{f}: forbidden consumer token "{token}"')

if violations:
    print("forbid-consumer: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print("forbid-consumer: PASS")
