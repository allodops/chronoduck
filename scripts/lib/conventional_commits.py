"""Shared Conventional Commits title pattern.

One shared constant (#166 DRY), imported by both scripts/pr-hygiene.py and
scripts/changelog.py, so the two can't drift apart.
"""

import re

# The JS original is anchored with `^` and has no trailing `$`, so it matches
# a valid Conventional Commits prefix at the start of the string with no
# requirement to consume the whole thing. `re.match` already anchors at the
# start of the string, matching that semantics; call `.match(text)` (not
# `.search`) to preserve it.
CONVENTIONAL_COMMITS_RE = re.compile(
    r"^(feat|fix|docs|style|refactor|perf|test|build|ci|chore|revert)(\([^)]+\))?!?: .+"
)
