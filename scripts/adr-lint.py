#!/usr/bin/env python3
"""make adr-lint

Enforces docs/decisions/*.md's shape (Article IX.1/IX.2): filename matches
\\d{4}-[a-z0-9-]+.md, numbers form one contiguous sequence from 0000 (the
template), front matter `status` is one of
proposed/accepted/deprecated/superseded with an ISO date, and a superseded
ADR names its successor. 0000-template.md is exempt from the front-matter
content checks (its values are placeholders, not a real decision record)
but still occupies its slot in the numbering sequence.
"""

import re
import sys
from pathlib import Path

import yaml

# PyYAML's default (YAML 1.1) resolver auto-converts an unquoted
# `date: 2026-09-04` into a datetime.date object, but the `isinstance(date,
# str)` check below (ISO_DATE_RE) needs the front matter's date to stay a
# plain string so a malformed date is rejected by regex instead of silently
# accepted as a valid datetime.date. A resolver with the timestamp
# implicit-tag removed keeps it a string without hand-rolling a YAML parser.
class _NoTimestampLoader(yaml.SafeLoader):
    pass


_NoTimestampLoader.yaml_implicit_resolvers = {
    k: [(tag, regexp) for tag, regexp in v if tag != "tag:yaml.org,2002:timestamp"]
    for k, v in yaml.SafeLoader.yaml_implicit_resolvers.items()
}

args = sys.argv[1:]
root_idx = args.index("--root") if "--root" in args else -1
root = Path(args[root_idx + 1]) if root_idx != -1 else Path.cwd()

FILENAME_RE = re.compile(r"^(\d{4})-([a-z0-9-]+)\.md$")
STATUSES = ["proposed", "accepted", "deprecated", "superseded"]
ISO_DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")

directory = root / "docs" / "decisions"
violations = []

if not directory.exists():
    print("adr-lint: FAIL", file=sys.stderr)
    print("  docs/decisions/ does not exist", file=sys.stderr)
    sys.exit(1)

files = [f.name for f in directory.iterdir() if f.name.endswith(".md")]
numbered = []

for file in files:
    m = FILENAME_RE.match(file)
    if not m:
        violations.append(f"{file}: filename does not match \\d{{4}}-[a-z0-9-]+.md")
        continue
    numbered.append({"file": file, "number": int(m.group(1))})

numbers = sorted(n["number"] for n in numbered)
for i in range(len(numbers)):
    if numbers[i] != i:
        found = str(numbers[i]).zfill(4) if i < len(numbers) else "nothing"
        violations.append(f"numbering gap or duplicate: expected {str(i).zfill(4)}, found {found}")
        break

FRONT_MATTER_RE = re.compile(r"^---\n([\s\S]*?)\n---")

for entry in numbered:
    file = entry["file"]
    if file == "0000-template.md":
        continue

    content = (directory / file).read_text(encoding="utf8")
    fm_match = FRONT_MATTER_RE.match(content)
    if not fm_match:
        violations.append(f"{file}: missing front matter")
        continue

    try:
        fm = yaml.load(fm_match.group(1), Loader=_NoTimestampLoader)
        if fm is None:
            fm = {}
    except yaml.YAMLError as e:
        violations.append(f"{file}: front matter is not valid YAML ({e})")
        continue
    if not isinstance(fm, dict):
        fm = {}

    status = fm.get("status")
    if status not in STATUSES:
        violations.append(f'{file}: status "{status}" is not one of {", ".join(STATUSES)}')

    date = fm.get("date") if isinstance(fm, dict) else None
    if not isinstance(date, str) or not ISO_DATE_RE.match(date):
        violations.append(f'{file}: date "{date}" is not an ISO date (YYYY-MM-DD)')

    if fm.get("status") == "superseded" and not fm.get("superseded_by"):
        violations.append(f'{file}: status is "superseded" but front matter has no "superseded_by" field naming its successor')

if violations:
    print("adr-lint: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print(f"adr-lint: PASS ({len(numbered)} ADR(s), numbered 0000-{str(len(numbered) - 1).zfill(4)})")
