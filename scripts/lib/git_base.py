"""Resolve a base ref to diff against, on a possibly-shallow CI checkout.

Shared by scripts/hygiene/constitution-check.py and
scripts/build-relevant-changed.py so neither duplicates this logic
(#166-style DRY).
"""

import subprocess


def _git(root, args):
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True)


def _try_resolve(root, candidate):
    return _git(root, ["rev-parse", "--verify", candidate]).returncode == 0


def _has_merge_base(root, candidate):
    return _git(root, ["merge-base", candidate, "HEAD"]).returncode == 0


# Resolves `explicit_base` if given, else tries local `origin/main`/`main`,
# else fetches `origin/main` explicitly (a PR-triggered CI checkout is
# typically shallow and only fetches the ref needed for the merge commit --
# `origin/main` may genuinely not be a resolvable local ref yet, not because
# there's no base to diff against). Once a base is found, ensures a
# merge-base with HEAD exists (deepening the checkout if not) so a
# three-dot diff -- the one that actually answers "did *this branch* change
# X," not "does X currently differ from base's live tip" -- is possible.
# Returns None if no base could be resolved at all.
def resolveBase(root, explicit_base=None):
    base = explicit_base or None
    if not base:
        for candidate in ("origin/main", "main"):
            if _try_resolve(root, candidate):
                base = candidate
                break
    if not base:
        # network/remote unavailable -- fall through to "no base" below
        _git(root, ["fetch", "origin", "main:refs/remotes/origin/main"])
        if _try_resolve(root, "origin/main"):
            base = "origin/main"
    if not base:
        return None

    if not _has_merge_base(root, base):
        # best effort -- the caller's diff fails closed if this didn't help
        if _git(root, ["fetch", "--unshallow"]).returncode != 0:
            _git(root, ["fetch", "--deepen=1000000"])
    return base


# Three-dot diff (relative to the merge-base), matching resolveBase's own
# reasoning above.
def changedFiles(root, base):
    result = _git(root, ["diff", "--name-only", f"{base}...HEAD"])
    if result.returncode != 0:
        raise RuntimeError(
            f"git -C {root} diff --name-only {base}...HEAD failed (exit {result.returncode}): {result.stderr.strip()}"
        )
    return [line for line in result.stdout.split("\n") if line]
