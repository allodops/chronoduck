"""Shared interactive-CLI `pr diff <n>` fetch for interactive scripts.

Run on the owner's machine, interactively (never from a CI workflow -- CI has
no interactively-configured GitHub CLI identity installed; scripts/lib/gh.py's
plain-`gh` helpers are the CI-safe equivalent). Used by both
scripts/pr-hygiene.py and scripts/hygiene/forbid-deferral.py -- one shared
fetch, including its rationale, rather than two independent copies (#166 DRY).

Net diff, not `--patch` (a per-commit patch series) -- the latter still shows
a line as "added" in an early commit's patch even after a subsequent commit
in the same PR removes it, false-positiving the deferral scan on something
that never reaches the target branch (#154).
"""

import os
import subprocess

# The interactive GitHub CLI identity: configured per-operator via the
# environment, outside tracked source, so no personal alias is ever a literal
# in this file. Defaults to plain `gh` for anyone without one configured.
GH_INTERACTIVE = os.environ.get("CHRONODUCK_GH_INTERACTIVE_CLI", "gh")


def fetchPrDiff(n):
    result = subprocess.run([GH_INTERACTIVE, "pr", "diff", str(n)], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"{GH_INTERACTIVE} pr diff {n} failed (exit {result.returncode}): {result.stderr.strip()}"
        )
    return result.stdout
