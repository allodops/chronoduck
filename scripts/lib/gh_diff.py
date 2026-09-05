"""Shared `gh-tsouza pr diff <n>` fetch for interactive scripts.

Run by a human/Claude Code on the owner's machine (never from a CI workflow --
CI has no gh-tsouza identity installed; scripts/lib/gh.py's plain-`gh` helpers
are the CI-safe equivalent). Used by both scripts/pr-hygiene.py and
scripts/hygiene/forbid-deferral.py -- one shared fetch, including its
rationale, rather than two independent copies (#166 DRY).

Net diff, not `--patch` (a per-commit patch series) -- the latter still shows
a line as "added" in an early commit's patch even after a subsequent commit
in the same PR removes it, false-positiving the deferral scan on something
that never reaches the target branch (#154).
"""

import subprocess


def fetchPrDiff(n):
    result = subprocess.run(["gh-tsouza", "pr", "diff", str(n)], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"gh-tsouza pr diff {n} failed (exit {result.returncode}): {result.stderr.strip()}"
        )
    return result.stdout
