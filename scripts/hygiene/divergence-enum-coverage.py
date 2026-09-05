#!/usr/bin/env python3
# make divergence-enum-coverage — issue #36's third meta-test: "divergence
# enum values no fixture exercises" (Article V.3).
#
# Known edge case in parseEnumMembers() below, not a bug to silently work
# around: it splits the enum body on "," up front, before stripping "//" /
# "/* */" comments out of each resulting piece — a comment containing a
# literal comma (or a block comment spanning a comma) is split apart first
# and only has its comment markers stripped from each half separately.
# Tightening that parsing edge case is a separate concern from this scan's
# own job (declared-divergence coverage) and is left alone here.
import os
import re
import sys

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()

SRC_DIR = os.path.join(root, "src")
FIXTURES_DIR = os.path.join(root, "test", "fixtures")
SQL_TEST_DIR = os.path.join(root, "test", "sql")


def list_files(dir_, pred):
    if not os.path.isdir(dir_):
        return []
    out = []

    def walk(d):
        for entry in os.listdir(d):
            full = os.path.join(d, entry)
            if os.path.isdir(full):
                walk(full)
            elif pred(entry):
                out.append(full)

    walk(dir_)
    return out


# `enum class <Name>Divergence { ... }` — a flat, comment-bearing C++ enum.
# No nested braces are expected inside a declared-divergence enum body, so a
# non-greedy match up to the first `}` is sufficient.
DIVERGENCE_ENUM_RE = re.compile(r"enum\s+class\s+([A-Za-z_][A-Za-z0-9_]*Divergence)\s*\{([\s\S]*?)\}")


def parse_enum_members(body):
    result = []
    for part in body.split(","):
        part = re.sub(r"//.*$", "", part, flags=re.MULTILINE)
        part = re.sub(r"/\*[\s\S]*?\*/", "", part)
        part = part.strip()
        if not part:
            continue
        name = part.split("=")[0].strip()
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            result.append(name)
    return result


def find_declared_divergences(root):
    enums = []  # {enumName, member, file}
    for file in list_files(SRC_DIR, lambda f: bool(re.search(r"\.(hpp|hh|h|cpp|cc|cxx)$", f))):
        try:
            with open(file, "rb") as fh:
                raw = fh.read()
        except OSError:
            continue
        if b"\0" in raw:
            continue
        content = raw.decode("utf-8", errors="replace")
        for m in DIVERGENCE_ENUM_RE.finditer(content):
            for member in parse_enum_members(m.group(2)):
                enums.append({"enumName": m.group(1), "member": member, "file": file[len(root) + 1:]})
    return enums


def is_exercised(member, corpus):
    return any(member in text for text in corpus)


declared = find_declared_divergences(root)
corpus_files = list_files(FIXTURES_DIR, lambda f: f.endswith(".yaml") or f.endswith(".yml")) + list_files(
    SQL_TEST_DIR, lambda f: f.endswith(".test")
)
corpus = []
for f in corpus_files:
    try:
        with open(f, "r", encoding="utf-8", errors="replace") as fh:
            corpus.append(fh.read())
    except OSError:
        corpus.append("")

violations = []
for d in declared:
    if not is_exercised(d["member"], corpus):
        violations.append(f'{d["file"]}: {d["enumName"]}::{d["member"]} is a declared divergence no fixture or sqllogictest exercises')

if violations:
    print("divergence-enum-coverage: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print(f"divergence-enum-coverage: PASS ({len(declared)} declared divergence(s))")
