#!/usr/bin/env python3
"""make lanes-check

Verifies .github/ci-lanes.json against the
actual workflow files (Article VII.2: "A lane is registered in
.github/ci-lanes.json or it is not a lane.").
"""

import json
import re
import sys
from pathlib import Path

import yaml


def _root_from_args(args):
    if "--root" in args:
        return Path(args[args.index("--root") + 1])
    return Path.cwd()


def main():
    args = sys.argv[1:]
    root = _root_from_args(args)

    violations = []

    registry_path = root / ".github" / "ci-lanes.json"
    if not registry_path.exists():
        print(f"lanes-check: FAIL\n  {registry_path} does not exist", file=sys.stderr)
        sys.exit(1)
    registry = json.loads(registry_path.read_text(encoding="utf8"))
    registered = {lane["context"]: lane for lane in registry["lanes"]}

    wf_dir = root / ".github" / "workflows"
    actual_jobs = {}  # context -> workflow filename
    continue_on_error_jobs = []

    if wf_dir.exists():
        for entry in sorted(wf_dir.iterdir(), key=lambda p: p.name):
            if not re.search(r"\.ya?ml$", entry.name):
                continue
            doc = yaml.safe_load(entry.read_text(encoding="utf8"))
            for job_id, job in (doc or {}).get("jobs", {}).items():
                actual_jobs[job_id] = entry.name
                job = job or {}
                if job.get("continue-on-error") is True:
                    continue_on_error_jobs.append(f"{entry.name}:{job_id}")
                for step in job.get("steps") or []:
                    step = step or {}
                    if step.get("continue-on-error") is True:
                        step_label = step.get("name") or step.get("uses") or step.get("run") or "?"
                        continue_on_error_jobs.append(f'{entry.name}:{job_id} (step "{step_label}")')

    # (a) registered context with no matching job
    for context, lane in registered.items():
        if context not in actual_jobs:
            violations.append(
                f'registered context "{context}" ({lane.get("workflow")}) has no job in the workflow files'
            )
        elif lane.get("workflow") and actual_jobs.get(context) != lane["workflow"]:
            violations.append(
                f'registered context "{context}" names workflow "{lane["workflow"]}" '
                f'but the job is actually in "{actual_jobs.get(context)}"'
            )

    # (b) a job exists that is not registered
    for context, workflow in actual_jobs.items():
        if context not in registered:
            violations.append(f'job "{context}" in {workflow} is not registered in {registry_path}')

    # (c) continue-on-error anywhere
    for loc in continue_on_error_jobs:
        violations.append(f"continue-on-error: true found at {loc} — a lane's red must never be silently tolerated")

    # (d) docs/testing/lanes.md, when present, must list exactly the registered lanes
    lanes_doc_path = root / "docs" / "testing" / "lanes.md"
    if lanes_doc_path.exists():
        doc = lanes_doc_path.read_text(encoding="utf8")
        registered_contexts = set(registered.keys())
        doc_contexts = set(re.findall(r"`([a-zA-Z0-9_-]+)`", doc))
        for c in registered_contexts:
            if c not in doc_contexts:
                violations.append(f'docs/testing/lanes.md is missing registered lane "{c}"')
        for c in doc_contexts:
            if c not in registered_contexts:
                violations.append(f'docs/testing/lanes.md lists "{c}", which is not a registered lane')

    if violations:
        print("lanes-check: FAIL", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        sys.exit(1)
    print("lanes-check: PASS")


if __name__ == "__main__":
    main()
