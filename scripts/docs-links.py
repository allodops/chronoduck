#!/usr/bin/env python3
"""make docs-links

Resolves every relative markdown link and #anchor under docs/ and
README.md: a relative path must exist, and an #anchor must match some
heading's generated slug in the target file (GitHub's algorithm —
lowercase, spaces to hyphens, strip anything but
letters/digits/hyphens/underscores). An http(s):// link is never checked (no
network access).

`slugify` and `headingSlugs` are kept as importable, side-effect-free
functions (guarded by `if __name__ == "__main__"` below) so
scripts/hygiene-selftest.py can import them directly for its own
pure-function unit assertions.
"""

import os
import re
import sys
from pathlib import Path

LINK_RE = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
HEADING_RE = re.compile(r"^#{1,6}\s+(.+)$")
NON_WORD_RE = re.compile(r"[^\w-]")
WHITESPACE_RE = re.compile(r"\s+")


def slugify(heading):
    s = heading.strip().lower()
    s = WHITESPACE_RE.sub("-", s)
    s = NON_WORD_RE.sub("", s)
    return s


def headingSlugs(markdown):
    slugs = set()
    for line in markdown.split("\n"):
        m = HEADING_RE.match(line)
        if m:
            slugs.add(slugify(m.group(1)))
    return slugs


def _listMarkdownFiles(directory, out=None):
    if out is None:
        out = []
    for entry in Path(directory).iterdir():
        if entry.is_dir():
            _listMarkdownFiles(entry, out)
        elif entry.name.endswith(".md"):
            out.append(entry)
    return out


def main():
    args = sys.argv[1:]
    root_idx = args.index("--root") if "--root" in args else -1
    root = Path(args[root_idx + 1]) if root_idx != -1 else Path.cwd()

    files = []
    docs_dir = root / "docs"
    if docs_dir.exists():
        _listMarkdownFiles(docs_dir, files)
    readme_path = root / "README.md"
    if readme_path.exists():
        files.append(readme_path)

    violations = []

    for file in files:
        content = file.read_text(encoding="utf8")
        rel = file.relative_to(root)
        for m in LINK_RE.finditer(content):
            target = m.group(1)
            if re.match(r"^https?://", target):
                continue

            # split() finds every "#" in target; only parts[0] (the path)
            # and parts[1] (the anchor) are kept, so a target with
            # two-or-more literal "#" characters silently drops everything
            # after the second one -- unlike a maxsplit=1 split, which
            # would rejoin everything after the first "#" into `anchor`.
            # That's the deliberate behavior here, not an oversight.
            if "#" in target:
                parts = target.split("#")
                path_part, anchor = parts[0], parts[1]
            else:
                path_part, anchor = target, None

            target_file = file
            if path_part:
                # os.path.normpath collapses ".."/"." purely lexically, with
                # no symlink resolution, so a link like "../foo.md" compares
                # equal to how the original file's own path would be
                # joined.
                target_file = Path(os.path.normpath(file.parent / path_part))
                if not target_file.exists():
                    violations.append(f'{rel}: dead link to "{path_part}"')
                    continue

            if anchor:
                target_content = content if target_file == file else target_file.read_text(encoding="utf8")
                if anchor not in headingSlugs(target_content):
                    violations.append(f'{rel}: dead anchor "#{anchor}" in "{path_part or "(same file)"}"')

    if violations:
        print("docs-links: FAIL", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        sys.exit(1)
    print(f"docs-links: PASS ({len(files)} file(s) scanned)")


if __name__ == "__main__":
    main()
