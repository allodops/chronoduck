#!/usr/bin/env python3
# oracle-fence — T5 (docs/testing/rules.md): "Oracles never import the
# kernel. The from-scratch evaluator and the fixture harness live in a
# separate build target whose include path cannot reach the extension's
# sources; a meta-test walks includes and fails on any edge." AGENTS.md's
# own "Where things are" table states the same rule for this repo:
# "test/oracle/ — the from-scratch oracle (must never include `src/`)."
#
# Walks every `#include "..."` (quoted, relative — never `#include <...>`,
# which is always a standard/system header and never a path in this
# repository) reachable from every file under `test/oracle/`, transitively,
# resolving each include relative to the including file's own directory
# (matching how a C++ compiler itself resolves a quoted include), and fails
# if any resolved path lands under `src/`.
import os
import re
import sys

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()

ORACLE_DIR = os.path.join(root, "test", "oracle")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
CPP_EXT_RE = re.compile(r"\.(hpp|hh|h|hxx|cpp|cc|cxx)$")


def list_files(dir_):
    if not os.path.isdir(dir_):
        return []
    out = []

    def walk(d):
        for entry in sorted(os.listdir(d)):
            full = os.path.join(d, entry)
            if os.path.isdir(full):
                walk(full)
            elif CPP_EXT_RE.search(entry):
                out.append(full)

    walk(dir_)
    return out


def quoted_includes(path):
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    return INCLUDE_RE.findall(content)


def to_posix_relative(from_root):
    return os.path.relpath(from_root, root).replace(os.sep, "/")


oracle_files = list_files(ORACLE_DIR)
if not oracle_files:
    print("oracle-fence: FAIL", file=sys.stderr)
    print(
        "  test/oracle has no .hpp/.cpp files to fence (Article V.2 / AGENTS.md: the from-scratch oracle lives there)",
        file=sys.stderr,
    )
    sys.exit(1)

violations = []
visited = set()


def walk(abs_path, chain):
    key = os.path.normpath(abs_path)
    if key in visited:
        return
    visited.add(key)
    if not os.path.exists(abs_path):
        return

    for inc in quoted_includes(abs_path):
        resolved = os.path.normpath(os.path.join(os.path.dirname(abs_path), inc))
        rel_path = to_posix_relative(resolved)
        next_chain = chain + [rel_path]
        if rel_path == "src" or rel_path.startswith("src/"):
            violations.append(f"{chain[0]}: reaches {rel_path} via {' -> '.join(next_chain)}")
            continue
        walk(resolved, next_chain)


for file in oracle_files:
    rel = to_posix_relative(file)
    walk(file, [rel])

if violations:
    print("oracle-fence: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print(f"oracle-fence: PASS ({len(oracle_files)} file(s) under test/oracle, no edge reaches src/)")
