#!/usr/bin/env python3
# The comparator is a single symbol in a single translation unit that no
# test file may shadow (docs/testing/comparator.md's closing paragraph):
# this scans test/ for the word "epsilon" — the term a fixed, ungrounded
# tolerance is named with — and fails if it appears anywhere other than the
# one file that documents the derivation's absence of one,
# test/kernel/comparator_test.cpp. `grep -r epsilon test/` finding only
# that file is this issue's second acceptance criterion; this script is the
# mechanically-enforced version of that same check.
import os
import re
import subprocess
import sys

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()
using_real_tree = "--root" not in args

WHITELISTED_PATH = "test/kernel/comparator_test.cpp"
FORBIDDEN_RE = re.compile(r"\bepsilon\b", re.IGNORECASE)


def list_files(dir_):
    if using_real_tree:
        out = subprocess.run(["git", "-C", dir_, "ls-files", "test"], capture_output=True, text=True).stdout
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

    walk(os.path.join(dir_, "test"), "test")
    return files


files = list_files(root)
violations = []

for f in files:
    if f == WHITELISTED_PATH:
        continue
    try:
        with open(os.path.join(root, f), "rb") as fh:
            raw = fh.read()
    except OSError:
        continue
    if b"\0" in raw:
        continue
    content = raw.decode("utf-8", errors="replace")
    lines = content.split("\n")
    for i, line in enumerate(lines):
        if FORBIDDEN_RE.search(line):
            violations.append(f"{f}:{i + 1}: forbidden tolerance token \"epsilon\" outside the comparator (Article V.3)")

if violations:
    print("forbid-test-tolerance: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print("forbid-test-tolerance: PASS")
