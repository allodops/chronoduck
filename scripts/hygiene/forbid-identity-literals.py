#!/usr/bin/env python3
# #263 removed every hardcoded local-CLI-alias/tool-name literal from source,
# but its own hygiene-selftest.py checks only structurally verified the 3
# files that issue actually touched. This is the permanent, tree-wide guard
# that a *new* script (or an untouched one) doesn't reintroduce either token
# anywhere else under the roots below — the same sibling-pattern shape as
# forbid-consumer.py/consumer-tokens.json (Article VI.1), split the same way
# into a scan and its own tokens file so the tokens themselves live in one
# place a human can audit without reading Python.
import json
import os
import subprocess
import sys

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()
using_real_tree = "--root" not in args

HERE = os.path.dirname(os.path.abspath(__file__))
TOKENS_PATH = os.path.join(HERE, "identity-literal-tokens.json")
with open(TOKENS_PATH, "r", encoding="utf-8") as f:
    _cfg = json.load(f)
TOKENS = _cfg["tokens"]
SCAN_ROOTS = _cfg["scanRoots"]
EXEMPT_PATHS = _cfg["exemptPaths"]


def read_text(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    if b"\0" in raw:
        return None
    return raw.decode("utf-8", errors="replace")


def list_files(dir_):
    if using_real_tree:
        out = subprocess.run(["git", "-C", dir_, "ls-files", *SCAN_ROOTS], capture_output=True, text=True).stdout
        return [l for l in out.split("\n") if l]
    files = []

    def walk(d, prefix):
        try:
            entries = os.listdir(d)
        except OSError:
            return
        for entry in entries:
            full = os.path.join(d, entry)
            rel = f"{prefix}/{entry}" if prefix else entry
            if os.path.isdir(full):
                walk(full, rel)
            else:
                files.append(rel)

    for scan_root in SCAN_ROOTS:
        walk(os.path.join(dir_, scan_root), scan_root.rstrip("/"))
    return files


def is_exempt(path):
    return any(path == p or path.startswith(p) for p in EXEMPT_PATHS)


# AGENTS.md, CLAUDE.md, CONSTITUTION.md, GOVERNANCE.md and CHANGELOG.md are
# the project's own deliberate process/narrative documents that legitimately
# name real tooling (#263's own out-of-scope list) — they all live at the
# repo root, outside every scanRoot above, so this scan structurally never
# reaches them and they need no exemptPaths entry of their own.
#
# The scanner never scans the file that configures it — the same fact
# forbid-consumer.py's SELF_PATH documents for its own tokens file, not a
# policy exemption, just what this tool's own input is.
SELF_PATH = os.path.relpath(TOKENS_PATH, root) if using_real_tree else None

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
    lines = content.split("\n")
    for i, line in enumerate(lines):
        for token in TOKENS:
            if token in line:
                violations.append(f'{f}:{i + 1}: forbidden identity literal "{token}"')

if violations:
    print("forbid-identity-literals: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print("forbid-identity-literals: PASS")
