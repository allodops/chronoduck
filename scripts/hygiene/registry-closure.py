#!/usr/bin/env python3
# registry-closure — Article V.1: "A function exists only through
# src/kernel/registry.def... A row missing from any roster fails the build,
# named after the row." Two closure directions, both against
# src/kernel/registry.def as the single source of truth:
#
#   1. every registry row has a matching test/sql/<name>.test file (a row
#      without one fails, naming the row) — issue #26's first acceptance
#      criterion.
#   2. no source file under src/ registers a kernel function ad-hoc, outside
#      a registry-driven Register_<name> function — issue #26's third
#      acceptance criterion ("a function in src/functions without a row is
#      red", implemented here against this repo's actual current layout,
#      where both real functions still live directly in
#      src/chronoduck_extension.cpp rather than under a src/functions/ tree).
#
# Parses registry.def with a plain regex over TS_FN(...) lines, matching
# this repo's other hygiene scans' style (a text/regex scan, not a real C
# preprocessor or AST — see forbid-consumer.py, workflow-shape.py).
import os
import re
import sys

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()

REGISTRY_DEF_PATH = os.path.join("src", "kernel", "registry.def")
REGISTRY_TYPES_PATH = os.path.join("src", "kernel", "registry_types.hpp")

EXCLUDED_PATHS = [REGISTRY_DEF_PATH, REGISTRY_TYPES_PATH]

TS_FN_ROW_RE = re.compile(r"^\s*TS_FN\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,", re.MULTILINE)


def list_files_under(dir_, prefix=""):
    files = []
    try:
        entries = os.listdir(os.path.join(dir_, prefix))
    except OSError:
        return files
    for entry in entries:
        rel = f"{prefix}/{entry}" if prefix else entry
        full = os.path.join(dir_, rel)
        if os.path.isdir(full):
            files.extend(list_files_under(dir_, rel))
        else:
            files.append(rel)
    return files


def parse_registry_row_names(root):
    def_path = os.path.join(root, REGISTRY_DEF_PATH)
    if not os.path.exists(def_path):
        return None
    with open(def_path, "r", encoding="utf-8") as f:
        content = f.read()
    return [m.group(1) for m in TS_FN_ROW_RE.finditer(content)]


def check_test_file_presence(root, names):
    violations = []
    for name in names:
        test_path = os.path.join(root, "test", "sql", f"{name}.test")
        if not os.path.exists(test_path):
            violations.append(f'row "{name}" has no test/sql/{name}.test')
    return violations


REGISTER_FN_START_RE = re.compile(
    r"(?:^|\n)[ \t]*(?:static\s+)?(?:void\s+)?Register_([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{]*\)\s*\{"
)


def find_matching_brace(content, open_brace_index):
    depth = 0
    i = open_brace_index
    n = len(content)
    while i < n:
        c = content[i]
        if c == "/" and i + 1 < n and content[i + 1] == "/":
            nl = content.find("\n", i)
            i = n if nl == -1 else nl
            continue
        if c == "/" and i + 1 < n and content[i + 1] == "*":
            end = content.find("*/", i + 2)
            i = n if end == -1 else end + 1
            continue
        if c in ('"', "'"):
            quote = c
            i += 1
            while i < n and content[i] != quote:
                if content[i] == "\\":
                    i += 1
                i += 1
            i += 1
            continue
        if c == "{":
            depth += 1
        if c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return n - 1


def mask_registry_driven_registrations(content, valid_names):
    masked = content
    valid_set = set(valid_names)
    spans = []
    for m in REGISTER_FN_START_RE.finditer(content):
        name = m.group(1)
        if name not in valid_set:
            continue
        open_brace = content.find("{", m.start())
        if open_brace == -1:
            continue
        close_brace = find_matching_brace(content, open_brace)
        spans.append((m.start(), close_brace + 1))
    for start, end in spans:
        region = masked[start:end]
        blanked = re.sub(r"[^\n]", " ", region)
        masked = masked[:start] + blanked + masked[end:]
    return masked


AD_HOC_REGISTRATION_RE = re.compile(r"\bloader\s*\.\s*Register(?:Function|AggregateFunction|TableFunction)\s*\(")


def check_no_ad_hoc_registration(root, names):
    violations = []
    files = [f"src/{f}" for f in list_files_under(os.path.join(root, "src"), "")]
    for f in files:
        if not re.search(r"\.(cpp|cc|cxx|hpp|hh|h)$", f):
            continue
        if f in EXCLUDED_PATHS:
            continue
        path = os.path.join(root, f)
        try:
            with open(path, "rb") as fh:
                raw = fh.read()
        except OSError:
            continue
        if b"\0" in raw:
            continue
        content = raw.decode("utf-8", errors="replace")

        masked = mask_registry_driven_registrations(content, names)
        lines = masked.split("\n")
        for i, line in enumerate(lines):
            if AD_HOC_REGISTRATION_RE.search(line):
                violations.append(f"{f}:{i + 1}: kernel function registered outside any registry-driven Register_<name> function")
    return violations


names = parse_registry_row_names(root)
if names is None:
    print("registry-closure: FAIL", file=sys.stderr)
    print(f"  {REGISTRY_DEF_PATH} does not exist", file=sys.stderr)
    sys.exit(1)

violations = check_test_file_presence(root, names) + check_no_ad_hoc_registration(root, names)

if violations:
    print("registry-closure: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print("registry-closure: PASS")
