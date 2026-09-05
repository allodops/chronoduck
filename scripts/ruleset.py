#!/usr/bin/env python3
"""make ruleset-add-check CONTEXT=<name> / make ruleset-remove-check CONTEXT=<name>

Port of scripts/ruleset.mjs. The `main` branch ruleset's
required_status_checks list is never edited any other way (Article
VII.3). Resolves the ruleset by name, refuses a context that isn't backed
by a registered merge-posture lane or hasn't reported green on main, edits
required_status_checks, PUTs the whole ruleset back.

Run interactively by a human/Claude Code (never by a CI workflow -- Article
VII.3), so this keeps its own gh-tsouza constant rather than importing
scripts/lib/gh.py's plain-`gh` helpers (those are the CI-safe equivalent
for standalone scripts like scripts/pr-label.py/scripts/issue-label-check.py);
only the REPO constant is imported, since that's just data, not a plain-`gh`
call.
"""

import json
import subprocess
import sys
from pathlib import Path

import yaml

from lib.gh import REPO

RULESET_NAME = "main"
GH = "gh-tsouza"

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def _run(args, **kwargs):
    result = subprocess.run(args, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(args)} failed (exit {result.returncode}): {result.stderr.strip()}")
    return result.stdout


def loadLanes():
    registry = json.loads((ROOT / ".github" / "ci-lanes.json").read_text(encoding="utf8"))
    return registry["lanes"]


# A registered lane's `context` is our own workflow job id. A reusable-
# workflow-call job (`uses:` a workflow, not a `run:` step) fans out into
# per-matrix-leg check-run names GitHub reports as "<job name> / <leg>" -- so
# a live context can belong to a lane either by exact match, or by having
# the lane's job `name:` (read from its workflow file) as a " / "-delimited
# prefix.
def findOwningLane(lanes, live_context):
    for lane in lanes:
        if lane["context"] == live_context:
            return lane
    for lane in lanes:
        wf_path = ROOT / ".github" / "workflows" / lane["workflow"]
        try:
            doc = yaml.safe_load(wf_path.read_text(encoding="utf8"))
        except Exception:  # noqa: BLE001 - mirrors the .mjs's catch-all
            continue
        job = (doc or {}).get("jobs", {}).get(lane["context"]) or {}
        job_name = job.get("name")
        if job_name and live_context.startswith(f"{job_name} / "):
            return lane
    return None


def hasReportedGreenOnMain(live_context):
    # Filter in Python, not via a jq expression built from untrusted input --
    # jq treats a bare identifier as a function call, not a string, unless
    # it's threaded through --arg, which `gh api --jq` (a single filter
    # string) has no clean way to accept.
    out = _run([GH, "api", f"repos/{REPO}/commits/main/check-runs", "--paginate", "--slurp"])
    pages = json.loads(out)
    check_runs = [run for page in pages for run in (page.get("check_runs") or [])]
    return any(run.get("name") == live_context and run.get("conclusion") == "success" for run in check_runs)


def getRuleset():
    out = _run([GH, "api", f"repos/{REPO}/rulesets"])
    rulesets = json.loads(out)
    found = next((r for r in rulesets if r["name"] == RULESET_NAME), None)
    if not found:
        raise RuntimeError(f'no ruleset named "{RULESET_NAME}" found')
    out = _run([GH, "api", f"repos/{REPO}/rulesets/{found['id']}"])
    return json.loads(out)


def putRuleset(ruleset):
    body = {
        "name": ruleset["name"],
        "target": ruleset["target"],
        "enforcement": ruleset["enforcement"],
        "conditions": ruleset["conditions"],
        "rules": ruleset["rules"],
    }
    tmp = ROOT / ".ruleset-put.tmp.json"
    tmp.write_text(json.dumps(body), encoding="utf8")
    try:
        _run([GH, "api", "-X", "PUT", f"repos/{REPO}/rulesets/{ruleset['id']}", "--input", str(tmp)])
    finally:
        tmp.unlink(missing_ok=True)


def main():
    args = sys.argv[1:]
    mode = args[0] if len(args) > 0 else None
    context = args[1] if len(args) > 1 else None
    if mode not in ("add", "remove") or not context:
        print("usage: ruleset.py <add|remove> <context>", file=sys.stderr)
        sys.exit(2)

    if mode == "add":
        lanes = loadLanes()
        lane = findOwningLane(lanes, context)
        if not lane:
            print(
                f'ruleset-add-check: FAIL\n  "{context}" is not backed by any registered lane in .github/ci-lanes.json',
                file=sys.stderr,
            )
            sys.exit(1)
        if lane.get("posture") != "merge":
            print(
                f'ruleset-add-check: FAIL\n  "{context}" belongs to lane "{lane["context"]}", '
                f'whose posture is "{lane.get("posture")}", not "merge"',
                file=sys.stderr,
            )
            sys.exit(1)
        if not hasReportedGreenOnMain(context):
            print(f'ruleset-add-check: FAIL\n  "{context}" has never reported green on main', file=sys.stderr)
            sys.exit(1)

        ruleset = getRuleset()
        rsc_rule = next((r for r in ruleset["rules"] if r["type"] == "required_status_checks"), None)
        if not rsc_rule:
            rsc_rule = {
                "type": "required_status_checks",
                "parameters": {"strict_required_status_checks_policy": True, "required_status_checks": []},
            }
            ruleset["rules"].append(rsc_rule)
        existing = rsc_rule["parameters"]["required_status_checks"]
        if any(c["context"] == context for c in existing):
            print(f'ruleset-add-check: "{context}" is already required')
            sys.exit(0)
        existing.append({"context": context})
        putRuleset(ruleset)
        print(f'ruleset-add-check: "{context}" is now required (lane "{lane["context"]}")')
    else:
        ruleset = getRuleset()
        rsc_rule = next((r for r in ruleset["rules"] if r["type"] == "required_status_checks"), None)
        if not rsc_rule or not any(c["context"] == context for c in rsc_rule["parameters"]["required_status_checks"]):
            print(f'ruleset-remove-check: "{context}" was not required')
            sys.exit(0)
        rsc_rule["parameters"]["required_status_checks"] = [
            c for c in rsc_rule["parameters"]["required_status_checks"] if c["context"] != context
        ]
        putRuleset(ruleset)
        print(f'ruleset-remove-check: "{context}" is no longer required')


if __name__ == "__main__":
    main()
