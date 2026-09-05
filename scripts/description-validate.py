#!/usr/bin/env python3
"""make description-validate

Validates docs/community/description.yml against the shape
scripts/vendor/description.schema.json documents (its own header, and the
sibling .source file, explain why that's a hand-derived encoding rather than
a vendored upstream file — duckdb/community-extensions ships no standalone
schema). Implements the checks directly rather than a generic JSON-Schema
interpreter, matching scripts/fixtures-validate.py's precedent (Article
IV.2's dependency-light-script preference). description.schema.json and its
.source sibling are read by neither this script nor any other consumer —
they document this hand-rolled validation, not drive it.
"""

import sys
from pathlib import Path

import yaml

args = sys.argv[1:]
root_idx = args.index("--root") if "--root" in args else -1
root = Path(args[root_idx + 1]) if root_idx != -1 else Path.cwd()
file_idx = args.index("--file") if "--file" in args else -1
file = Path(args[file_idx + 1]) if file_idx != -1 else root / "docs" / "community" / "description.yml"


def validate(doc):
    violations = []

    extension = doc.get("extension")
    if not extension or not isinstance(extension, dict):
        violations.append('missing required top-level key "extension"')
    elif not extension.get("name"):
        violations.append('missing required field "extension.name"')

    repo = doc.get("repo")
    if not repo or not isinstance(repo, dict):
        violations.append('missing required top-level key "repo"')
    else:
        if not repo.get("github"):
            violations.append('missing required field "repo.github"')
        if not repo.get("ref"):
            violations.append('missing required field "repo.ref"')

    # `.get()` can't distinguish an absent key from an explicit `maintainers:
    # null` (both return None); an explicit null is a real authoring mistake
    # -- the field is present but has the wrong type -- and should be
    # flagged rather than silently treated as "the field is absent", so key
    # presence is checked explicitly rather than the looked-up value's
    # truthiness.
    maintainers_present = isinstance(extension, dict) and "maintainers" in extension
    maintainers = extension.get("maintainers") if maintainers_present else None
    if maintainers_present and not isinstance(maintainers, list):
        violations.append('"extension.maintainers" must be an array')

    return violations


if not file.exists():
    print(f"description-validate: FAIL\n  {file}: not found", file=sys.stderr)
    sys.exit(1)

try:
    doc = yaml.safe_load(file.read_text(encoding="utf8"))
except yaml.YAMLError as e:
    print(f"description-validate: FAIL\n  {file}: could not parse YAML ({e})", file=sys.stderr)
    sys.exit(1)

violations = validate(doc or {})
if violations:
    print("description-validate: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {file}: {v}", file=sys.stderr)
    sys.exit(1)
print(f"description-validate: PASS ({file})")
