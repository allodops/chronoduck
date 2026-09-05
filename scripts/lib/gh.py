"""Shared plain-`gh`-CLI helper for scripts that run standalone in CI.

Port of scripts/lib/gh.mjs. No Claude Code session, no `gh-tsouza` alias
available there -- the one deliberate exception to this project's "always
gh-tsouza" rule, which is about interactive sessions on the owner's machine,
not scripts GitHub Actions itself executes. Never call the gh*() functions
below from a script meant to run interactively (scripts/ruleset.py -- once
ported -- is that case and keeps its own gh-tsouza constant; it is never
invoked by a workflow, only by a human or Claude Code locally, per
Article VII.3) -- importing the REPO constant alone is fine, since it's just
data, not a plain-`gh` call.
"""

import json
import subprocess

REPO = "allodops/chronoduck"
GH = "gh"


def _run(args):
    result = subprocess.run([GH, *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"{GH} {' '.join(args)} failed (exit {result.returncode}): {result.stderr.strip()}"
        )
    return result.stdout


# A GET with query-string fields. `gh api` defaults to POST once any `-f`
# field is given, so `-X GET` must be explicit or it silently tries to
# create the resource instead of listing it.
def ghGetPaginated(path, fields=None):
    fields = fields or {}
    field_flags = []
    for k, v in fields.items():
        field_flags += ["-f", f"{k}={v}"]
    out = _run(["api", "-X", "GET", f"repos/{REPO}{path}", *field_flags, "--paginate", "--slurp"])
    pages = json.loads(out)
    flat = []
    for page in pages:
        flat.extend(page)
    return flat


def ghGet(path):
    out = _run(["api", f"repos/{REPO}{path}"])
    return json.loads(out)


def ghAddLabels(issue_or_pr_number, labels):
    flags = []
    for label in labels:
        flags += ["-f", f"labels[]={label}"]
    _run(["api", f"repos/{REPO}/issues/{issue_or_pr_number}/labels", *flags])
