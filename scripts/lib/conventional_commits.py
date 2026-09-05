"""Shared Conventional Commits title pattern.

Port of scripts/lib/conventional-commits.mjs (#166 DRY: previously duplicated
identically, with no shared constant, across scripts/pr-hygiene.mjs and
scripts/changelog.mjs -- undrifted so far, but nothing enforced it staying
that way). Both import this one constant now, so drift is structurally
impossible rather than merely accidental.

The identifier is kept exactly as in the .mjs original (not renamed to a
lowercase Python module attribute) so the import surface #241-#244 depend on
stays literally unchanged during the mechanical caller port (issue #240's own
acceptance criteria).
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
