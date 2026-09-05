#!/usr/bin/env python3
# make tier-coverage-floor — issue #36's fourth meta-test: "a per-tier
# coverage floor, raise-only, refusing zero"
# (REGRESSED/STALE/UNTRACKED/ZERO-FLOOR/UNKNOWN-PRIMITIVE).
import json
import os
import re
import sys

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()

PRIMITIVES_MD = os.path.join(root, "docs", "testing", "primitives.md")
KERNEL_TEST_DIR = os.path.join(root, "test", "kernel")
FLOOR_PATH = os.path.join(root, "test", "kernel", "tier-coverage-floor.json")


def parse_tier_primitives(text):
    tiers = {}
    current = None
    for line in text.split("\n"):
        heading = re.match(r"^## Tier (\d+)\b", line)
        if heading:
            current = int(heading.group(1))
            tiers[current] = []
            continue
        if re.match(r"^##\s", line) and not heading:
            current = None
            continue
        if current is None or not line.startswith("|"):
            continue
        if re.match(r"^\|\s*Primitive\s*\|", line):
            continue
        if re.match(r"^\|[-\s|]+\|$", line):
            continue
        cells = line.split("|")
        first_cell = cells[1].strip() if len(cells) > 1 else None
        if first_cell:
            tiers[current].append(first_cell)
    return tiers


if not os.path.exists(PRIMITIVES_MD):
    print("tier-coverage-floor: FAIL", file=sys.stderr)
    print(f"  {PRIMITIVES_MD} does not exist", file=sys.stderr)
    sys.exit(1)
with open(PRIMITIVES_MD, "r", encoding="utf-8") as f:
    tier_primitives = parse_tier_primitives(f.read())


def joined_comment_block(content):
    lines = content.split("\n")
    parts = []
    for line in lines:
        m = re.match(r"^\s*//\s?(.*)$", line)
        if not m:
            break
        parts.append(m.group(1))
    return " ".join(parts)


TIER_CITATION_RE = re.compile(r"Tier (\d+)((?:\s*(?:,|and)?\s*`[^`]+`)+)\s+rows?\b")
BACKTICK_RE = re.compile(r"`([^`]+)`")


def parse_citations(joined):
    citations = []
    for m in TIER_CITATION_RE.finditer(joined):
        tier = int(m.group(1))
        for bm in BACKTICK_RE.finditer(m.group(2)):
            citations.append({"tier": tier, "primitive": bm.group(1)})
    return citations


try:
    test_files = [f for f in os.listdir(KERNEL_TEST_DIR) if f.endswith("_test.cpp")]
except OSError:
    test_files = []

covered = {}  # tier -> {primitive: True}, an insertion-ordered set (mirrors JS Set iteration order)
unknown_citations = []  # {file, tier, primitive}

for file in test_files:
    with open(os.path.join(KERNEL_TEST_DIR, file), "r", encoding="utf-8") as f:
        content = f.read()
    joined = joined_comment_block(content)
    for c in parse_citations(joined):
        tier, primitive = c["tier"], c["primitive"]
        known = tier_primitives.get(tier, [])
        if primitive not in known:
            unknown_citations.append({"file": file, "tier": tier, "primitive": primitive})
            continue
        covered.setdefault(tier, {})[primitive] = True

floor = {}
if os.path.exists(FLOOR_PATH):
    with open(FLOOR_PATH, "r", encoding="utf-8") as f:
        floor = json.load(f)

violations = []

for key, floor_count in floor.items():
    tier = int(re.sub(r"^Tier ", "", key))
    current_count = len(covered.get(tier, {}))
    if floor_count == 0:
        violations.append(f'ZERO-FLOOR: "{key}" is recorded at 0 in test/kernel/tier-coverage-floor.json — a zero floor asserts nothing; remove the entry instead until the tier has real coverage')
        continue
    if current_count < floor_count:
        violations.append(f'REGRESSED: "{key}" covers {current_count} primitive(s) now, below its committed floor of {floor_count}')
    elif current_count > floor_count:
        violations.append(f'STALE: "{key}" covers {current_count} primitive(s) now, above its committed floor of {floor_count} — raise test/kernel/tier-coverage-floor.json to {current_count}')

for tier, names in covered.items():
    key = f"Tier {tier}"
    if key not in floor and len(names) > 0:
        violations.append(f'UNTRACKED: "{key}" now covers {len(names)} primitive(s) ({", ".join(names)}) but has no entry in test/kernel/tier-coverage-floor.json — add "{key}": {len(names)}')

for uc in unknown_citations:
    violations.append(
        f'UNKNOWN-PRIMITIVE: test/kernel/{uc["file"]} cites Tier {uc["tier"]} `{uc["primitive"]}` row, which is not a primitive docs/testing/primitives.md\'s Tier {uc["tier"]} table lists'
    )

if violations:
    print("tier-coverage-floor: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print(f"tier-coverage-floor: PASS ({len(floor)} tracked tier(s))")
