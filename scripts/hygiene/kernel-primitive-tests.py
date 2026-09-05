#!/usr/bin/env python3
# Compiles and runs every test/kernel/*_test.cpp — the L1a direct test for
# each dependency-free Tier 0-5 primitive translation unit under
# src/kernel/ — with a bare g++, and fails if any one of them fails to
# compile, exits nonzero, or doesn't print its own "<name>: PASS" sentinel.
# This is what makes each primitive's direct test "provably executed by an
# unconditional, failure-propagating lane" (Article V.5): `make hygiene`
# runs in the required "hygiene" CI job on every PR, and this scan is one of
# its checks, exactly like every other scripts/hygiene/*.py scan.
#
# #229: the compile invocation passes `-I <root>/src` so each test's own
# quoted `#include "kernel/<name>.hpp"` resolves the same way
# `src/chronoduck_extension.cpp` already resolves its own `#include
# "kernel/<name>.hpp"` — src/ is this tree's established header root, not
# the repo root — instead of via a fragile `../../src/kernel/<name>.hpp`
# climb out of test/kernel/. `-I <root>` is kept alongside it so
# `oracle_sweep_test.cpp`'s `#include "test/oracle/<name>.hpp"` keeps
# resolving root-relative. `forbid-relative-kernel-include.py` is the
# mechanical scan that keeps the fragile `../` form from coming back.
import os
import subprocess
import sys
import tempfile

args = sys.argv[1:]
root = args[args.index("--root") + 1] if "--root" in args else os.getcwd()

KERNEL_TEST_DIR = os.path.join(root, "test", "kernel")


def run(cmd):
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.stdout, proc.stderr, proc.returncode


try:
    files = sorted(f for f in os.listdir(KERNEL_TEST_DIR) if f.endswith("_test.cpp"))
except OSError:
    print("kernel-primitive-tests: FAIL", file=sys.stderr)
    print(f"  {KERNEL_TEST_DIR} does not exist", file=sys.stderr)
    sys.exit(1)

if not files:
    print("kernel-primitive-tests: FAIL", file=sys.stderr)
    print(f"  no *_test.cpp files found under {KERNEL_TEST_DIR}", file=sys.stderr)
    sys.exit(1)

tmp = tempfile.mkdtemp(prefix="kernel-primitive-tests-")
violations = []

for file in files:
    test_path = os.path.join(KERNEL_TEST_DIR, file)
    name = file[: -len(".cpp")]
    bin_path = os.path.join(tmp, name)

    out, err, code = run(
        ["g++", "-std=c++17", "-Wall", "-Wextra", "-I", os.path.join(root, "src"), "-I", root, test_path, "-o", bin_path]
    )
    if code != 0:
        violations.append(f"test/kernel/{file} failed to compile:\n{out}{err}")
        continue

    ran_out, ran_err, ran_code = run([bin_path])
    sentinel = f"{name}: PASS"
    if ran_code != 0 or sentinel not in ran_out:
        violations.append(f"test/kernel/{file} did not pass:\n{ran_out}{ran_err}")
        continue
    print(f"kernel-primitive-tests: PASS — test/kernel/{file}")

if violations:
    print("kernel-primitive-tests: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print("kernel-primitive-tests: PASS")
