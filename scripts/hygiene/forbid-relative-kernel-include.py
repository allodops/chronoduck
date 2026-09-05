#!/usr/bin/env python3
# #229: test/kernel/*.cpp used to reach its own kernel headers via a
# fragile parent-relative include (`#include "../../src/kernel/foo.hpp"`) —
# the only reason that ever compiled is that kernel-primitive-tests.py's
# g++ invocation passed no -I flag at all, so a header could only resolve
# relative to the including file itself. Now that invocation passes
# `-I <root>/src`, every test/kernel/*.cpp reaches its own kernel headers
# with a clean, bare quoted include (`#include "kernel/foo.hpp"`) — the same
# form `src/chronoduck_extension.cpp` already used before #229 ever touched
# this tree, since src/ (not the repo root) is this codebase's established
# header root. `-I <root>` stays alongside it purely for
# `oracle_sweep_test.cpp`'s `#include "test/oracle/foo.hpp"`, a
# cross-top-level-directory reference with no `src/` precedent to follow.
# Either way, a parent-relative include anywhere under test/kernel/ is
# always a regression back to the fragile pre-#229 form. This scan fails
# the moment one reappears, naming the offending file and line — the
# mechanical, permanent half of #229's fix, mirroring
# forbid-test-tolerance.py's shape for the same directory.
import os
import re
import subprocess
import sys

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()
using_real_tree = "--root" not in args

KERNEL_TEST_DIR = "test/kernel"
FORBIDDEN_RE = re.compile(r'#include\s*"\.\./')


def list_files(dir_):
    if using_real_tree:
        out = subprocess.run(
            ["git", "-C", dir_, "ls-files", KERNEL_TEST_DIR], capture_output=True, text=True
        ).stdout
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

    walk(os.path.join(dir_, KERNEL_TEST_DIR), KERNEL_TEST_DIR)
    return files


files = list_files(root)
violations = []

for f in files:
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
            violations.append(f"{f}:{i + 1}: parent-relative include reintroduces the fragile pre-#229 form")

if violations:
    print("forbid-relative-kernel-include: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print("forbid-relative-kernel-include: PASS")
