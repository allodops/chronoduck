#!/usr/bin/env python3
import os
import re
import sys

import yaml

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()

WF_DIR = os.path.join(root, ".github", "workflows")
violations = []

STEP_ALLOWED_KEYS = {"env", "name", "if", "run", "shell", "working-directory"}

if os.path.isdir(WF_DIR):
    for entry in os.listdir(WF_DIR):
        if not re.search(r"\.ya?ml$", entry):
            continue
        path = os.path.join(WF_DIR, entry)
        try:
            with open(path, "r", encoding="utf-8") as f:
                doc = yaml.safe_load(f)
        except Exception as e:
            violations.append(f"{entry}: could not parse YAML ({e})")
            continue

        jobs = (doc or {}).get("jobs") or {}
        if "permissions" not in (doc or {}):
            jobs_missing_permissions = [name for name, job in jobs.items() if "permissions" not in (job or {})]
            if jobs_missing_permissions:
                violations.append(
                    f"{entry}: no top-level `permissions:`, and job(s) {', '.join(jobs_missing_permissions)} declare none either"
                )

        for job_name, job in jobs.items():
            job_keys = list((job or {}).keys())
            if len(job_keys) == 1 and job_keys[0] == "uses":
                continue

            steps = (job or {}).get("steps") or []
            for step in steps:
                if "run" not in step:
                    continue
                step_keys = [k for k in step.keys() if k not in STEP_ALLOWED_KEYS]
                if step_keys:
                    violations.append(
                        f'{entry}: job "{job_name}" step "{step.get("name", "(unnamed)")}" has unexpected keys: {", ".join(step_keys)}'
                    )
                run = str(step.get("run") or "").strip()
                if not re.match(r"^make\s+\S", run):
                    violations.append(
                        f'{entry}: job "{job_name}" step "{step.get("name", "(unnamed)")}" runs "{run}", which is not `make <target>`'
                    )

if violations:
    print("workflow-shape: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print("workflow-shape: PASS")
