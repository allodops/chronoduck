#!/usr/bin/env python3
"""Scan a PR diff's added lines for deferral language lacking an issue reference.

Named with an underscore (not a hyphen, unlike its sibling scans under
scripts/hygiene/) because scripts/hygiene-selftest.py imports
scanDiffForDeferral() from it directly for its pure-function unit
assertions — a hyphenated name cannot be `import`ed by name in Python. This
is the same "hyphenated for a standalone CLI entry point, underscored for
an importable module" convention scripts/lib/*.py already established (PR
#249's own Deviations section); this file is both at once (a CLI entry
point AND an importable module), so it follows the importable-module half
of that convention.
"""
import os
import re
import sys

SCOPE_PREFIXES = ["src/", "test/", "scripts/"]
DEFERRAL_RE = re.compile(r"\b(later|for now|temporary|will be|not yet|follow-up|TBD)\b", re.IGNORECASE)

# A real "#<issue>" reference: `#` immediately followed by digits, not
# glued to another `#` or word character on its left (so the comment
# marker's own leading digits on a line like "#123 quick and dirty, no
# real cleanup plan" can't masquerade as a citation just because a bare
# "#\d+" regex is unanchored — #166). Tested against the comment text
# *after* the marker is stripped, so the marker character itself is never a
# candidate match in the first place.
ISSUE_REF_RE = re.compile(r"(?:^|[^A-Za-z0-9#])#\d+\b")


def comment_marker_for(path):
    if re.search(r"\.(mjs|cpp|cc|cxx|hpp|hh|h|c)$", path):
        return "//"
    if re.search(r"\.(test|sql|yml|yaml)$", path):
        return "#"
    return None


def tail_words(text, n):
    words = [w for w in text.strip().split() if w]
    return " ".join(words[-n:]) if n > 0 else ""


def head_words(text, n):
    words = [w for w in text.strip().split() if w]
    return " ".join(words[:n])


# Deferral language is an excuse to skip something, and belongs in prose —
# comments — not in a string or regex literal. Two behaviors below are
# deliberate: a deferral phrase split across a wrapped two-line comment
# (`// this is a stopgap for` / `// now, will replace it`) is still caught,
# by joining the previous comment line's tail words to this line's head
# words before matching; and `prev` is reset to None on any line that isn't
# itself a continuing comment in scope, so an unrelated adjacent line can
# never be mistaken for part of the same comment (the #178 regression).
def scanDiffForDeferral(diff):
    lines = diff.split("\n")
    current_file = None
    violations = []
    prev = None  # {added, afterMarker} of the previous added comment line, if immediately preceding

    for line in lines:
        if line.startswith("+++ "):
            path = line[4:].strip()
            current_file = path[2:] if path.startswith("b/") else path
            prev = None
            continue
        if not line.startswith("+") or line.startswith("+++"):
            prev = None
            continue
        if not current_file or not any(current_file.startswith(p) for p in SCOPE_PREFIXES):
            prev = None
            continue

        added = line[1:]
        marker = comment_marker_for(current_file)
        marker_at = added.find(marker) if marker else -1
        if marker_at == -1:
            prev = None
            continue
        after_marker = added[marker_at + len(marker):]

        flagged = False
        if DEFERRAL_RE.search(after_marker) and not ISSUE_REF_RE.search(after_marker):
            violations.append(f'{current_file}: added line(s) use deferral language without "#<issue>": {added.strip()}')
            flagged = True
        elif (
            not flagged
            and prev is not None
            and not DEFERRAL_RE.search(prev["afterMarker"])
            and not DEFERRAL_RE.search(after_marker)
            and DEFERRAL_RE.search(f"{tail_words(prev['afterMarker'], 4)} {head_words(after_marker, 4)}")
            and not ISSUE_REF_RE.search(after_marker)
        ):
            violations.append(f'{current_file}: added line(s) use deferral language without "#<issue>": {prev["added"].strip()} {added.strip()}')

        prev = {"added": added, "afterMarker": after_marker}
    return violations


if __name__ == "__main__":
    args = sys.argv[1:]
    pr_idx = args.index("--pr") if "--pr" in args else -1
    diff_file_idx = args.index("--diff-file") if "--diff-file" in args else -1

    if pr_idx == -1 and diff_file_idx == -1:
        print("forbid-deferral: usage: forbid_deferral.py (--pr <n> | --diff-file <path>)", file=sys.stderr)
        sys.exit(2)

    if diff_file_idx != -1:
        with open(args[diff_file_idx + 1], "r", encoding="utf-8") as f:
            diff = f.read()
    else:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
        from lib.gh_diff import fetchPrDiff

        diff = fetchPrDiff(args[pr_idx + 1])

    violations = scanDiffForDeferral(diff)
    if violations:
        print("forbid-deferral: FAIL", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        sys.exit(1)
    print("forbid-deferral: PASS")
