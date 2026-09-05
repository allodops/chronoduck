#!/usr/bin/env python3
"""make coverage-check

Verifies docs/design/coverage.md's
matrix still holds, per issue #25's acceptance criteria: (a) every
K-disposition row names a real construct, (b) every milestone-shaped token
cited in the matrix (a "(class N)" cross-reference, or a "T<major>.<minor>"
plan-key) is real, (c) every K+ row names a row or option now present, (d)
every P row names a concrete planned construct.

src/kernel/registry.def does not exist yet at M1 (Article V.1: "This
article is enforced from the PR that creates registry.def"), so "names a
real registry row" cannot mean "appears in registry.def". Per this issue,
it instead means: the row's "How" column names a construct that appears in
docs/design/primitives.md's Tier table or docs/design/architecture.md's
state-class/edge-mode/value-domain vocabulary (an identifier verified
against a real design doc), OR — since most K rows cite the *registry*
function name itself (e.g. "rate", "ts_of_last_change_over_time"), which
primitives.md deliberately does not enumerate (it lists the ~70 primitives
registry rows compose from, not the ~115 rows themselves) — the row names
at least one identifier-shaped, underscore-bearing token, tolerated as a
forward reference to the not-yet-created registry.def. A row with neither
(empty, or pure prose with no identifiable construct) fails.

The "(class N)" self-reference check is fully offline. The "T<major>.<minor>"
plan-key check shells out to the configurable interactive GitHub CLI's
`issue list --search` to confirm the token names a real issue's
`<!-- plan-key: T#.# -->` comment, mirroring forbid-ledger.py's
issueIsOpen() pattern: when a token exists to verify and the CLI is
unavailable, that is reported as a violation, not silently skipped
(Article II.3's "offline runs report this check as red only when such a
comment is present to verify" generalized to plan-key tokens).
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

# The interactive GitHub CLI identity: configured per-operator via the
# environment, outside tracked source, so no personal alias is ever a literal
# in this file. Defaults to plain `gh` for anyone without one configured.
GH_INTERACTIVE = os.environ.get("CHRONODUCK_GH_INTERACTIVE_CLI", "gh")

args = sys.argv[1:]
root_idx = args.index("--root") if "--root" in args else -1
root = Path(args[root_idx + 1]) if root_idx != -1 else Path.cwd()

DESIGN_DIR = root / "docs" / "design"
COVERAGE_PATH = DESIGN_DIR / "coverage.md"

if not COVERAGE_PATH.exists():
    print("coverage-check: FAIL", file=sys.stderr)
    print(f"  {COVERAGE_PATH} does not exist", file=sys.stderr)
    sys.exit(1)

# -- vocabulary: identifiers named for real in the design docs -------------
#
# "The actual registry/kernel design docs" (issue #25) is every doc under
# docs/design/ other than coverage.md itself (primitives.md is the one the
# issue names explicitly, since it's the concrete stand-in for the
# not-yet-existing registry.def, but architecture.md's state classes/edge
# modes and schema.md's profile/role vocabulary are equally real designed
# constructs a K row can legitimately cite).
#
# Two extraction passes: backtick-quoted spans (split on non-identifier
# separators — spans are often compound: "hist_add / hist_sub",
# "Grid{start,end,step}", "reduce_fold<Op>") and heading words (a doc
# section titled "## The profile" makes "profile" a real, named concept even
# where the word itself is never wrapped in backticks). Matching is
# case-sensitive: coverage.md quotes edge modes/state classes in the same
# uppercase the design docs use, and lowercasing would make common English
# words in headings ("the", "and") collide with prose everywhere.
STOPWORDS = {"left", "right", "count", "first", "last", "any", "new", "one", "two", "t", "v", "st", "ts", "the", "and", "for", "with"}


def identifiers_from_backtick_spans(text):
    out = set()
    for m in re.finditer(r"`([^`]+)`", text):
        for tok in re.split(r"[^A-Za-z0-9_]+", m.group(1)):
            if len(tok) < 3 or tok.lower() in STOPWORDS:
                continue
            out.add(tok)
    return out


def identifiers_from_headings(text):
    out = set()
    for line in text.split("\n"):
        if not line.startswith("#"):
            continue
        for tok in re.split(r"[^A-Za-z0-9_]+", re.sub(r"^#+\s*", "", line)):
            if len(tok) < 4 or tok.lower() in STOPWORDS:
                continue
            out.add(tok)
    return out


design_vocab = set()
if DESIGN_DIR.exists():
    for entry in DESIGN_DIR.iterdir():
        if not entry.name.endswith(".md") or entry.name == "coverage.md":
            continue
        text = entry.read_text(encoding="utf8")
        design_vocab |= identifiers_from_backtick_spans(text)
        design_vocab |= identifiers_from_headings(text)

# An identifier-shaped, underscore-bearing token — the shape of a registry
# function name (rate_over_time, hist_detect_reset, ts_of_last_change_over_time)
# absent from registry.def, tolerated as a forward reference. No trailing
# word-boundary assertion: a row's How column can end a token with a suffix
# like "series_time_decayed_*" (a wildcard family name), where "_" keeps
# \b from firing right after the identifier's last letter.
FORWARD_REF_RE = re.compile(r"\b[a-z][a-z0-9]*_[a-z0-9]+")


def names_real_construct(text):
    for vocab_term in design_vocab:
        # word-boundary-ish containment check, case-sensitive
        if vocab_term in text:
            return vocab_term
    return None


# -- parse coverage.md into class sections and table rows ------------------

coverage_text = COVERAGE_PATH.read_text(encoding="utf8")
lines = coverage_text.split("\n")

class_headings = set()  # every "## N. Title" number
rows = []  # {classNum, primitive, appearsIn, where, how, lineNo}
current_class = None

CLASS_HEADING_RE = re.compile(r"^## (\d+)\.\s")
HEADER_ROW_RE = re.compile(r"^\|\s*Primitive\s*\|")
SEPARATOR_ROW_RE = re.compile(r"^\|[-\s|]+\|$")

for i, line in enumerate(lines):
    heading_match = CLASS_HEADING_RE.match(line)
    if heading_match:
        current_class = int(heading_match.group(1))
        class_headings.add(current_class)
        continue
    if not line.startswith("|"):
        continue
    if HEADER_ROW_RE.match(line):
        continue  # header row
    if SEPARATOR_ROW_RE.match(line):
        continue  # separator row
    cells = [c.strip() for c in line.split("|")[1:-1]]
    if len(cells) != 4:
        continue  # not a data row of this table shape
    primitive, appears_in, where, how = cells
    if primitive == "" and appears_in == "" and where == "" and how == "":
        continue
    rows.append({"classNum": current_class, "primitive": primitive, "appearsIn": appears_in, "where": where, "how": how, "lineNo": i + 1})

violations = []
verified_count = 0
tolerated_count = 0

# (a) + (c): every K and K+ row names a real construct or, failing that, a
# forward-reference-shaped identifier.
for row in rows:
    if row["where"] != "K" and row["where"] != "K+":
        continue
    haystack = f'{row["primitive"]} {row["how"]}'
    hit = names_real_construct(haystack)
    if hit:
        verified_count += 1
        continue
    if FORWARD_REF_RE.search(haystack):
        tolerated_count += 1
        continue
    violations.append(
        f'coverage.md:{row["lineNo"]}: {row["where"]} row "{row["primitive"]}" (class {row["classNum"]}) names no identifiable construct in its "How" column — not in docs/design/primitives.md, docs/design/architecture.md, and no registry-function-shaped identifier either'
    )

# (d): every P row names a concrete planned construct (non-trivial "How").
for row in rows:
    if row["where"] != "P":
        continue
    haystack = f'{row["primitive"]} {row["how"]}'
    if not FORWARD_REF_RE.search(haystack) and not names_real_construct(haystack):
        violations.append(f'coverage.md:{row["lineNo"]}: P row "{row["primitive"]}" (class {row["classNum"]}) names no concrete planned construct in its "How" column')

# (b) part 1: every "(class N)" cross-reference resolves to a real heading.
for row in rows:
    haystack = f'{row["appearsIn"]} {row["how"]}'
    for m in re.finditer(r"\bclass (\d+)\b", haystack):
        n = int(m.group(1))
        if n not in class_headings:
            violations.append(f'coverage.md:{row["lineNo"]}: "{row["primitive"]}" cites "class {n}", which is not a heading in this document')

# (b) part 2: every "T<major>.<minor>" plan-key token names a real issue.
plan_key_tokens = {m.group(0) for m in re.finditer(r"\bT\d+\.\d+\b", coverage_text)}


def issue_exists_for_plan_key(token):
    try:
        result = subprocess.run(
            [GH_INTERACTIVE, "issue", "list", "--state", "all", "--search", f'"plan-key: {token}" in:body', "--json", "number"],
            capture_output=True,
            text=True,
            check=True,
        )
        parsed = json.loads(result.stdout)
        return isinstance(parsed, list) and len(parsed) > 0
    except Exception:
        return None  # interactive GitHub CLI unavailable


for token in plan_key_tokens:
    exists = issue_exists_for_plan_key(token)
    if exists is None:
        violations.append(f'coverage.md: could not verify plan-key "{token}" ({GH_INTERACTIVE} unavailable)')
    elif not exists:
        violations.append(f'coverage.md: cites plan-key "{token}", which no issue\'s "<!-- plan-key: {token} -->" comment names')

if violations:
    print("coverage-check: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print(
    f"coverage-check: PASS ({len(rows)} row(s); {verified_count} K/K+ row(s) verified against a real construct, {tolerated_count} tolerated as forward references to registry.def; {len(class_headings)} class heading(s); {len(plan_key_tokens)} plan-key token(s) verified)"
)
