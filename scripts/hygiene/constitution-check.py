#!/usr/bin/env python3
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from lib.git_base import resolveBase as resolveBaseShared, changedFiles as changedFilesShared  # noqa: E402

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()
base = args[args.index("--base") + 1] if "--base" in args else None


def git(cmd_args):
    return subprocess.run(["git", "-C", root, *cmd_args], capture_output=True, text=True)


def version_of(text):
    m = re.search(r"\*\*Version\*\*\s+([\d.]+)", text)
    return m.group(1) if m else None


def last_amended_of(text):
    m = re.search(r"\*\*Last amended\*\*\s+([^\n*]+)", text)
    return m.group(1).strip() if m else None


def version_greater(a, b):
    pa = [int(x) for x in a.split(".")]
    pb = [int(x) for x in b.split(".")]
    for i in range(max(len(pa), len(pb))):
        x = pa[i] if i < len(pa) else 0
        y = pb[i] if i < len(pb) else 0
        if x != y:
            return x > y
    return False


def article_sections(text):
    header_re = re.compile(r"^## Article\s+(\S+)", re.MULTILINE)
    matches = list(header_re.finditer(text))
    sections = {}
    for i, m in enumerate(matches):
        start = m.start()
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        sections[m.group(1)] = text[start:end].rstrip()
    return sections


def changed_articles(old_text, new_text):
    old_sections = article_sections(old_text)
    new_sections = article_sections(new_text)
    changed = []
    for id_, body in new_sections.items():
        if old_sections.get(id_) != body:
            changed.append(id_)
    for id_ in old_sections:
        if id_ not in new_sections:
            changed.append(id_)
    return changed


base = resolveBaseShared(root, base)
if not base:
    print("constitution-check: FAIL (no base ref to diff against — cannot verify CONSTITUTION.md)", file=sys.stderr)
    sys.exit(1)

try:
    changed_files = changedFilesShared(root, base)
except RuntimeError:
    print(
        f"constitution-check: FAIL (no merge-base between {base} and HEAD even after attempting to fetch full history — cannot verify CONSTITUTION.md)",
        file=sys.stderr,
    )
    sys.exit(1)

if "CONSTITUTION.md" not in changed_files:
    print("constitution-check: PASS (CONSTITUTION.md unchanged)")
    sys.exit(0)

violations = []

old_result = git(["show", f"{base}:CONSTITUTION.md"])
old_text = old_result.stdout if old_result.returncode == 0 else ""

head_result = git(["show", "HEAD:CONSTITUTION.md"])
if head_result.returncode == 0:
    new_text = head_result.stdout
else:
    with open(os.path.join(root, "CONSTITUTION.md"), "r", encoding="utf-8") as f:
        new_text = f.read()

old_version = version_of(old_text)
new_version = version_of(new_text)
if not new_version or (old_version and not version_greater(new_version, old_version)):
    violations.append(f'CONSTITUTION.md changed but Version did not increase ({old_version or "none"} -> {new_version or "none"})')

new_amended = last_amended_of(new_text)
if not new_amended:
    violations.append("CONSTITUTION.md changed but has no Last amended date")

changed_article_ids = changed_articles(old_text, new_text)

diff_result = git(["diff", "--name-status", f"{base}...HEAD", "--", "docs/decisions/"])
added_adrs = [
    line[2:]
    for line in diff_result.stdout.split("\n")
    if line.startswith("A\t")
]

has_accepted_adr = False
references_changed_article = False
for adr in added_adrs:
    text_result = git(["show", f"HEAD:{adr}"])
    text = text_result.stdout
    if re.search(r"status:\s*accepted", text):
        has_accepted_adr = True
        if any(re.search(rf"Article\s+{re.escape(id_)}\b", text) for id_ in changed_article_ids):
            references_changed_article = True

if not has_accepted_adr:
    violations.append("CONSTITUTION.md changed but no new docs/decisions/*.md with `status: accepted` was added")
elif changed_article_ids and not references_changed_article:
    violations.append(
        f"CONSTITUTION.md changed Article(s) {', '.join(changed_article_ids)} but no new accepted ADR references any of them"
    )

if violations:
    print("constitution-check: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print("constitution-check: PASS")
