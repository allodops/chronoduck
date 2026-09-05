#!/usr/bin/env python3
import os
import re
import subprocess
import sys

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()
using_real_tree = "--root" not in args


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


# A citation is a backtick-fenced "path:construct:" immediately followed by a
# backtick-fenced expression naming the cited construct in that file.
# A line-number-style citation (the construct name is all digits) is always forbidden.
CITATION_RE = re.compile(r"`([^`\s:]+\.[A-Za-z0-9_]+):([^`:]+):`\s*`([^`]+)`")

PARTNER_BUILD_PREFIX = "build/partners/"

COMMENT_LINE_RE = re.compile(r"^\s*//\s?(.*)$")


def comment_runs(lines):
    runs = []
    current = None
    for i, line in enumerate(lines):
        m = COMMENT_LINE_RE.match(line)
        if m:
            if current is None:
                current = {"startLine": i, "parts": []}
            current["parts"].append(m.group(1))
        elif current is not None:
            runs.append(current)
            current = None
    if current is not None:
        runs.append(current)
    return runs


files = list_files(root)
violations = []
skipped_partner_citations = []

for f in files:
    try:
        content = read_text(os.path.join(root, f))
    except OSError:
        continue
    if content is None:
        continue

    lines = content.split("\n")
    seen = set()

    def check_citation(line_no, cited_file, construct, expr):
        key = f"{line_no}:{cited_file}:{construct}:{expr}"
        if key in seen:
            return
        seen.add(key)
        if re.fullmatch(r"\d+", construct):
            violations.append(
                f'{f}:{line_no}: line-number citation "{cited_file}:{construct}:" is forbidden — cite a construct, never a line number'
            )
            return
        cited_path = os.path.join(root, cited_file)
        if not os.path.exists(cited_path):
            if cited_file.startswith(PARTNER_BUILD_PREFIX):
                skipped_partner_citations.append(f'{f}:{line_no}: "{cited_file}" not built in this environment')
                return
            violations.append(f'{f}:{line_no}: citation references "{cited_file}", which does not exist')
            return
        with open(cited_path, "rb") as fh:
            cited_content = fh.read().decode("utf-8", errors="replace")
        occurrences = cited_content.count(expr)
        if occurrences != 1:
            violations.append(
                f"{f}:{line_no}: citation expression `{expr}` occurs {occurrences} times in {cited_file}, expected exactly 1"
            )

    for i, line in enumerate(lines):
        for m in CITATION_RE.finditer(line):
            check_citation(i + 1, m.group(1), m.group(2), m.group(3))

    for run in comment_runs(lines):
        if len(run["parts"]) < 2:
            continue
        joined = " ".join(run["parts"])
        for m in CITATION_RE.finditer(joined):
            check_citation(run["startLine"] + 1, m.group(1), m.group(2), m.group(3))

if violations:
    print("verify-citations: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
if skipped_partner_citations:
    print(f"verify-citations: PASS ({len(skipped_partner_citations)} citation(s) skipped — not built in this environment)")
    for s in skipped_partner_citations:
        print(f"  SKIPPED: {s}")
else:
    print("verify-citations: PASS")
