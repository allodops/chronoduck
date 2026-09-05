#!/usr/bin/env python3
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Paths are relative to this file's own directory (scripts/), not implicitly
# rooted under scripts/hygiene/ — most scans live there, but fixtures-validate
# (#194) lives at scripts/fixtures-validate.py, schema.json's sibling per
# #146, so it's named without the "hygiene/" prefix like every other entry.
#
# Each entry is (interpreter, relative path). Every scan below is a Python
# port; fixtures-validate.py lives outside scripts/hygiene/ (per issue #241's
# own text, it was out of that port's scope) but was itself ported to Python
# by a later issue in this milestone (#242) -- run under python3 like every
# other entry now that the Makefile/CI cutover (#245) means nothing in this
# list can fall back on the prior toolchain being present.
TREE_SCANS = [
    ("python3", "hygiene/forbid-ledger.py"),
    ("python3", "hygiene/forbid-consumer.py"),
    ("python3", "hygiene/verify-citations.py"),
    ("python3", "hygiene/workflow-shape.py"),
    ("python3", "hygiene/constitution-check.py"),
    ("python3", "hygiene/registry-closure.py"),
    ("python3", "hygiene/forbid-test-tolerance.py"),
    ("python3", "hygiene/forbid-relative-kernel-include.py"),
    ("python3", "hygiene/kernel-primitive-tests.py"),
    ("python3", "hygiene/oracle-fence.py"),
    ("python3", "hygiene/shape-roster.py"),
    ("python3", "hygiene/kernel-fixture-loader.py"),
    ("python3", "hygiene/derivation-sync.py"),
    ("python3", "hygiene/registry-roster-closure.py"),
    ("python3", "hygiene/divergence-enum-coverage.py"),
    ("python3", "hygiene/tier-coverage-floor.py"),
    ("python3", "fixtures-validate.py"),
]

failed = False
for interpreter, scan in TREE_SCANS:
    proc = subprocess.run([interpreter, os.path.join(HERE, scan)], capture_output=True, text=True)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        failed = True

if failed:
    print("hygiene: FAIL (one or more scans failed above)", file=sys.stderr)
    sys.exit(1)
print("hygiene: PASS")
