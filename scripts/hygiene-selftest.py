#!/usr/bin/env python3
import base64
import importlib
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
FIXTURES = os.path.join(ROOT, "test", "hygiene-fixtures")

# scripts/hygiene.py (this file's own orchestrator sibling) and
# scripts/hygiene/ (the scan directory) share the name "hygiene" — Python's
# import system resolves a same-directory "hygiene.py" module ahead of a
# "hygiene/" namespace package for that name, so `import hygiene.forbid_deferral`
# would find the wrong one. Importing directly from scripts/hygiene/ (added
# to sys.path on its own, ahead of the module-vs-package collision) sidesteps
# that rather than renaming either file to work around it.
sys.path.insert(0, os.path.join(HERE, "hygiene"))
from forbid_deferral import scanDiffForDeferral  # noqa: E402


def run(cmd, cwd=None, input_=None):
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, input=input_)
    return proc.stdout, proc.stderr, proc.returncode


def sh(cwd, cmd):
    # cmd is a trusted, fixed shell command string built only from this file's own literals.
    subprocess.run(f"git -c protocol.file.allow=always -C {cwd} {cmd}", shell=True, capture_output=True, text=True)


def git_q(cwd, *args):
    subprocess.run(["git", "-C", cwd, *args], capture_output=True, text=True)


def git_init(dir_, branch):
    os.makedirs(dir_, exist_ok=True)
    sh(dir_, f"init -q -b {branch}")
    sh(dir_, "config user.email test@example.com")
    sh(dir_, "config user.name test")


def write_file(path, content):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)


def copy_file(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(src, "rb") as fsrc, open(dst, "wb") as fdst:
        fdst.write(fsrc.read())


failures = 0


def expect_red(label, cmd):
    global failures
    out, err, code = run(cmd)
    if code == 0:
        print(f"SELFTEST FAIL: {label} was expected to fail (red) on its fixture but exited 0", file=sys.stderr)
        print(out, err, file=sys.stderr)
        failures += 1
    else:
        print(f"SELFTEST ok: {label} correctly red on its fixture")


def expect_green(label, cmd):
    global failures
    out, err, code = run(cmd)
    if code != 0:
        print(f"SELFTEST FAIL: {label} was expected to pass (green) on its fixture but exited {code}", file=sys.stderr)
        print(out, err, file=sys.stderr)
        failures += 1
    else:
        print(f"SELFTEST ok: {label} correctly green on its fixture")


# A fixture manifest (test/hygiene-fixtures/<scan>.json: {relPath: content}) is
# materialized into a disposable temp directory, never committed as literal
# git-tracked files that would match the real-tree scan it's designed to trip.
def materialize(manifest_name):
    with open(os.path.join(FIXTURES, f"{manifest_name}.json"), "r", encoding="utf-8") as f:
        manifest = json.load(f)
    tmp = tempfile.mkdtemp(prefix=f"{manifest_name}-selftest-")
    for rel_path, content_b64 in manifest.items():
        full = os.path.join(tmp, rel_path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(base64.b64decode(content_b64))
    return tmp


def py(*rel_parts):
    return os.path.join(HERE, *rel_parts)


# 1-4: the filesystem-rooted tree scans, materialized from JSON manifests.
expect_red("forbid-ledger", ["python3", py("hygiene", "forbid-ledger.py"), "--root", materialize("forbid-ledger")])
expect_red("forbid-consumer", ["python3", py("hygiene", "forbid-consumer.py"), "--root", materialize("forbid-consumer")])

# forbid-identity-literals: #265 — a reintroduced forbidden identity literal
# (scripts/hygiene/identity-literal-tokens.json) under a scanned root is red;
# a clean tree, including one where a token only appears inside an exempted
# historical ADR, is green.
expect_red(
    "forbid-identity-literals (reintroduced forbidden identity literal)",
    ["python3", py("hygiene", "forbid-identity-literals.py"), "--root", materialize("forbid-identity-literals-red")],
)
expect_green(
    "forbid-identity-literals (clean tree; token inside an exempted historical ADR doesn't trip it)",
    ["python3", py("hygiene", "forbid-identity-literals.py"), "--root", materialize("forbid-identity-literals-green")],
)
expect_red("verify-citations", ["python3", py("hygiene", "verify-citations.py"), "--root", materialize("verify-citations")])

# verify-citations: #47 — a citation into build/partners/ (a build artifact
# scripts/partners/rawduck-build.py makes at build time, never committed) is
# SKIPPED, not a violation, when that path doesn't exist in the materialized
# root; checked STRICTLY, exactly like any other citation, when it does —
# isolated across three fixtures so neither half of that rule can hide a
# false pass/fail in the other.
expect_green(
    "verify-citations (build/partners/ citation skipped when the path doesn't exist)",
    ["python3", py("hygiene", "verify-citations.py"), "--root", materialize("verify-citations-partner-absent-green")],
)
expect_red(
    "verify-citations (build/partners/ citation checked strictly when the path exists and mismatches)",
    ["python3", py("hygiene", "verify-citations.py"), "--root", materialize("verify-citations-partner-present-mismatch-red")],
)
expect_green(
    "verify-citations (build/partners/ citation checked strictly when the path exists and matches)",
    ["python3", py("hygiene", "verify-citations.py"), "--root", materialize("verify-citations-partner-present-match-green")],
)

expect_red("workflow-shape", ["python3", py("hygiene", "workflow-shape.py"), "--root", materialize("workflow-shape")])

# 4b: registry-closure (Article V.1) — three fixtures materialized from JSON
# manifests, matching the other filesystem-rooted tree scans above.
expect_red(
    "registry-closure (row missing test/sql/<name>.test)",
    ["python3", py("hygiene", "registry-closure.py"), "--root", materialize("registry-closure-missing-test")],
)
expect_red(
    "registry-closure (ad-hoc registration outside Register_<name>)",
    ["python3", py("hygiene", "registry-closure.py"), "--root", materialize("registry-closure-adhoc")],
)
expect_green(
    "registry-closure (every registration properly wrapped)",
    ["python3", py("hygiene", "registry-closure.py"), "--root", materialize("registry-closure-green")],
)

# 4c: the (state, det, scale_kind) static_assert rule — proven by actually
# compiling a standalone C++ fixture with a bare g++, not merely asserted in
# prose (pure C++/g++, no scripting language involved).
red_path = os.path.join(ROOT, "test", "hygiene-fixtures", "registry-static-assert-red.cpp")
out, err, code = run(["g++", "-std=c++17", "-c", red_path, "-o", "/dev/null"])
combined = out + err
if code == 0:
    print("SELFTEST FAIL: registry-static-assert (red fixture) was expected to fail to compile but exited 0", file=sys.stderr)
    failures += 1
elif "SLICE+D0+SUM_ABS must be rejected by IsValidRow" not in combined:
    print(f"SELFTEST FAIL: registry-static-assert (red fixture) failed, but not on the expected static_assert message:\n{combined}", file=sys.stderr)
    failures += 1
else:
    print("SELFTEST ok: registry-static-assert (red fixture) correctly fails to compile on SLICE+D0+SUM_ABS")

green_path = os.path.join(ROOT, "test", "hygiene-fixtures", "registry-static-assert-green.cpp")
out, err, code = run(["g++", "-std=c++17", "-c", green_path, "-o", "/dev/null"])
if code != 0:
    print(f"SELFTEST FAIL: registry-static-assert (green fixture) was expected to compile cleanly but failed:\n{out}{err}", file=sys.stderr)
    failures += 1
else:
    print("SELFTEST ok: registry-static-assert (green fixture) correctly compiles on SLICE+D1+SUM_ABS")

# 4d: forbid-test-tolerance (Article V.3 / #27) — a stray "epsilon" token
# under test/, outside the one whitelisted comparator test file, is red.
expect_red("forbid-test-tolerance", ["python3", py("hygiene", "forbid-test-tolerance.py"), "--root", materialize("forbid-test-tolerance")])

# 4d-2: forbid-relative-kernel-include (#229) — a reintroduced parent-relative
# include under test/kernel/ is red; the clean, bare quoted form is green.
expect_red(
    "forbid-relative-kernel-include",
    ["python3", py("hygiene", "forbid-relative-kernel-include.py"), "--root", materialize("forbid-relative-kernel-include-red")],
)
expect_green(
    "forbid-relative-kernel-include (clean bare-quoted include)",
    ["python3", py("hygiene", "forbid-relative-kernel-include.py"), "--root", materialize("forbid-relative-kernel-include-green")],
)

# 4e: kernel-primitive-tests (#27, generalized by #28/T1.4) — a broken
# test/kernel/comparator_test.cpp is red when it's the only *_test.cpp file
# in the materialized root.
expect_red(
    "kernel-primitive-tests",
    ["python3", py("hygiene", "kernel-primitive-tests.py"), "--root", materialize("comparator-test-broken")],
)

# 4e-2: oracle-fence (#42, T5).
expect_red("oracle-fence (test/oracle/*.hpp includes src/ directly)", ["python3", py("hygiene", "oracle-fence.py"), "--root", materialize("oracle-fence")])
expect_green(
    "oracle-fence (same-directory + system includes only)",
    ["python3", py("hygiene", "oracle-fence.py"), "--root", materialize("oracle-fence-green")],
)

# 4e-3: shape-roster (#42, T7).
expect_red(
    "shape-roster (VANISHED: roster'd shape no longer in registry.def)",
    ["python3", py("hygiene", "shape-roster.py"), "--root", materialize("shape-roster-vanished")],
)
expect_red(
    "shape-roster (REGRESSED: roster'd shape lost its worked-example citation)",
    ["python3", py("hygiene", "shape-roster.py"), "--root", materialize("shape-roster-regressed")],
)
expect_red(
    "shape-roster (ARRIVED-FAILING: new registry shape, no worked example)",
    ["python3", py("hygiene", "shape-roster.py"), "--root", materialize("shape-roster-arrived-failing")],
)
expect_red(
    "shape-roster (UNRECORDED: worked example exists, roster.json not updated)",
    ["python3", py("hygiene", "shape-roster.py"), "--root", materialize("shape-roster-unrecorded")],
)
expect_red(
    "shape-roster (UNKNOWN-SHAPE: citation names a shape registry.def doesn't declare)",
    ["python3", py("hygiene", "shape-roster.py"), "--root", materialize("shape-roster-unknown-shape")],
)
expect_green(
    "shape-roster (registry, citation and roster all agree)",
    ["python3", py("hygiene", "shape-roster.py"), "--root", materialize("shape-roster-green")],
)

# 4e-4: parity-roster (#44, T7) — L8's operator == aggregate parity leg,
# ratcheted the same way shape-roster is.
expect_red(
    "parity-roster (VANISHED: roster'd row no longer a D0 RANGE/HIST registry row)",
    ["python3", py("hygiene", "parity-roster.py"), "--root", materialize("parity-roster-vanished")],
)
expect_red(
    "parity-roster (REGRESSED: roster'd row lost its L8-PARITY citation)",
    ["python3", py("hygiene", "parity-roster.py"), "--root", materialize("parity-roster-regressed")],
)
expect_red(
    "parity-roster (ARRIVED-FAILING: new D0 RANGE/HIST row, no parity citation)",
    ["python3", py("hygiene", "parity-roster.py"), "--root", materialize("parity-roster-arrived-failing")],
)
expect_red(
    "parity-roster (UNRECORDED: parity citation exists, roster.json not updated)",
    ["python3", py("hygiene", "parity-roster.py"), "--root", materialize("parity-roster-unrecorded")],
)
expect_red(
    "parity-roster (UNKNOWN-ROW: citation names a row that isn't a D0 RANGE/HIST row)",
    ["python3", py("hygiene", "parity-roster.py"), "--root", materialize("parity-roster-unknown-row")],
)
expect_green(
    "parity-roster (registry, citation and roster all agree)",
    ["python3", py("hygiene", "parity-roster.py"), "--root", materialize("parity-roster-green")],
)

# 4f: the comparator headroom pin's static_assert mechanism — pure C++/g++.
for name, expect_message in [
    ("comparator-headroom-accept-red", "a reorder factor eroded to the accept edge must fail the floor"),
    ("comparator-headroom-reject-red", "a reorder factor eroded toward the divergence edge must fail the floor"),
]:
    path = os.path.join(ROOT, "test", "hygiene-fixtures", f"{name}.cpp")
    out, err, code = run(["g++", "-std=c++17", "-c", path, "-o", "/dev/null"])
    combined = out + err
    if code == 0:
        print(f"SELFTEST FAIL: {name} was expected to fail to compile but exited 0", file=sys.stderr)
        failures += 1
    elif expect_message not in combined:
        print(f"SELFTEST FAIL: {name} failed, but not on the expected static_assert message:\n{combined}", file=sys.stderr)
        failures += 1
    else:
        print(f"SELFTEST ok: {name} correctly fails to compile")

green_path = os.path.join(ROOT, "test", "hygiene-fixtures", "comparator-headroom-green.cpp")
out, err, code = run(["g++", "-std=c++17", "-c", green_path, "-o", "/dev/null"])
if code != 0:
    print(f"SELFTEST FAIL: comparator-headroom-green was expected to compile cleanly but failed:\n{out}{err}", file=sys.stderr)
    failures += 1
else:
    print("SELFTEST ok: comparator-headroom-green correctly compiles with the real kReorderFactor")

# 5: constitution-check needs a real git history — build one from the base/head fixture files.
tmp = tempfile.mkdtemp(prefix="cc-selftest-")
git_init(tmp, "main")
copy_file(os.path.join(FIXTURES, "constitution-check", "base", "CONSTITUTION.md"), os.path.join(tmp, "CONSTITUTION.md"))
sh(tmp, "add CONSTITUTION.md")
sh(tmp, 'commit -q -m base')
sh(tmp, "checkout -q -b head")
copy_file(os.path.join(FIXTURES, "constitution-check", "head", "CONSTITUTION.md"), os.path.join(tmp, "CONSTITUTION.md"))
sh(tmp, "add CONSTITUTION.md")
sh(tmp, 'commit -q -m "change without bumping version"')
expect_red("constitution-check", ["python3", py("hygiene", "constitution-check.py"), "--root", tmp, "--base", "main"])

# 5b: #172 — the newly-added accepted ADR must actually be about the Article
# that changed, not just any accepted ADR.
tmp = tempfile.mkdtemp(prefix="cc-topicality-selftest-")
git_init(tmp, "main")
copy_file(os.path.join(FIXTURES, "constitution-check", "topicality-base", "CONSTITUTION.md"), os.path.join(tmp, "CONSTITUTION.md"))
sh(tmp, "add CONSTITUTION.md")
sh(tmp, 'commit -q -m base')

sh(tmp, "checkout -q -b head-unrelated")
copy_file(os.path.join(FIXTURES, "constitution-check", "topicality-head", "CONSTITUTION.md"), os.path.join(tmp, "CONSTITUTION.md"))
copy_file(
    os.path.join(FIXTURES, "constitution-check", "topicality-adr-unrelated.md"),
    os.path.join(tmp, "docs", "decisions", "0016-widgets-are-blue.md"),
)
sh(tmp, "add CONSTITUTION.md docs/decisions/0016-widgets-are-blue.md")
sh(tmp, 'commit -q -m "amend Article II, attach unrelated ADR"')
expect_red(
    "constitution-check (accepted ADR unrelated to changed Article)",
    ["python3", py("hygiene", "constitution-check.py"), "--root", tmp, "--base", "main"],
)

sh(tmp, "checkout -q main")
sh(tmp, "checkout -q -b head-matching")
copy_file(os.path.join(FIXTURES, "constitution-check", "topicality-head", "CONSTITUTION.md"), os.path.join(tmp, "CONSTITUTION.md"))
copy_file(
    os.path.join(FIXTURES, "constitution-check", "topicality-adr-matching.md"),
    os.path.join(tmp, "docs", "decisions", "0016-gadgets-are-hexagonal.md"),
)
sh(tmp, "add CONSTITUTION.md docs/decisions/0016-gadgets-are-hexagonal.md")
sh(tmp, 'commit -q -m "amend Article II, attach matching ADR"')
expect_green(
    "constitution-check (accepted ADR references the changed Article)",
    ["python3", py("hygiene", "constitution-check.py"), "--root", tmp, "--base", "main"],
)

# 5c: #172 regression — appending a brand-new trailing article must not
# spuriously mark its untouched preceding neighbor as "changed" too.
tmp = tempfile.mkdtemp(prefix="cc-topicality-append-selftest-")
git_init(tmp, "main")
copy_file(os.path.join(FIXTURES, "constitution-check", "topicality-append-base", "CONSTITUTION.md"), os.path.join(tmp, "CONSTITUTION.md"))
sh(tmp, "add CONSTITUTION.md")
sh(tmp, 'commit -q -m base')

sh(tmp, "checkout -q -b head-neighbor-only")
copy_file(os.path.join(FIXTURES, "constitution-check", "topicality-append-head", "CONSTITUTION.md"), os.path.join(tmp, "CONSTITUTION.md"))
copy_file(
    os.path.join(FIXTURES, "constitution-check", "topicality-append-adr-neighbor-only.md"),
    os.path.join(tmp, "docs", "decisions", "0016-gadgets-are-hexagonal.md"),
)
sh(tmp, "add CONSTITUTION.md docs/decisions/0016-gadgets-are-hexagonal.md")
sh(tmp, 'commit -q -m "append Article III, attach ADR naming only the untouched neighbor"')
expect_red(
    "constitution-check (appended article — ADR naming only the untouched neighbor is still unrelated)",
    ["python3", py("hygiene", "constitution-check.py"), "--root", tmp, "--base", "main"],
)

sh(tmp, "checkout -q main")
sh(tmp, "checkout -q -b head-matching")
copy_file(os.path.join(FIXTURES, "constitution-check", "topicality-append-head", "CONSTITUTION.md"), os.path.join(tmp, "CONSTITUTION.md"))
copy_file(
    os.path.join(FIXTURES, "constitution-check", "topicality-append-adr-matching.md"),
    os.path.join(tmp, "docs", "decisions", "0016-sprockets-are-new.md"),
)
sh(tmp, "add CONSTITUTION.md docs/decisions/0016-sprockets-are-new.md")
sh(tmp, 'commit -q -m "append Article III, attach matching ADR"')
expect_green(
    "constitution-check (appended article — ADR naming the new article passes)",
    ["python3", py("hygiene", "constitution-check.py"), "--root", tmp, "--base", "main"],
)

# 5c: #181 — build-relevant-changed correctly classifies a src/ change as
# relevant and a docs-only change as not.
tmp = tempfile.mkdtemp(prefix="brc-selftest-")
git_init(tmp, "main")
write_file(os.path.join(tmp, "docs", "readme.md"), "base\n")
sh(tmp, "add docs/readme.md")
sh(tmp, 'commit -q -m base')

sh(tmp, "checkout -q -b docs-only")
write_file(os.path.join(tmp, "docs", "readme.md"), "docs-only change\n")
sh(tmp, "add docs/readme.md")
sh(tmp, 'commit -q -m "docs-only change"')
out, _, code = run(["python3", py("build-relevant-changed.py"), "--root", tmp, "--base", "main"])
if code != 0 or "BUILD_RELEVANT=false" not in out:
    print(f"SELFTEST FAIL: build-relevant-changed (docs-only) expected BUILD_RELEVANT=false, got: {out}", file=sys.stderr)
    failures += 1
else:
    print("SELFTEST ok: build-relevant-changed (docs-only) correctly reports not relevant")

sh(tmp, "checkout -q main")
sh(tmp, "checkout -q -b src-change")
write_file(os.path.join(tmp, "src", "thing.cpp"), "// changed\n")
sh(tmp, "add src/thing.cpp")
sh(tmp, 'commit -q -m "src change"')
out, _, code = run(["python3", py("build-relevant-changed.py"), "--root", tmp, "--base", "main"])
if code != 0 or "BUILD_RELEVANT=true" not in out:
    print(f"SELFTEST FAIL: build-relevant-changed (src/ change) expected BUILD_RELEVANT=true, got: {out}", file=sys.stderr)
    failures += 1
else:
    print("SELFTEST ok: build-relevant-changed (src/ change) correctly reports relevant")

# 6: forbid-deferral, diff-based — the fixture diff text is base64-encoded in
# the manifest so its trigger words never exist as plaintext in a
# git-tracked file for any scan (this one included) to stumble on.
with open(os.path.join(FIXTURES, "forbid-deferral.json"), "r", encoding="utf-8") as f:
    _fd_manifest = json.load(f)
_fd_diff = _fd_manifest["diff"]
tmp = tempfile.mkdtemp(prefix="fd-selftest-")
diff_file = os.path.join(tmp, "diff.patch")
with open(diff_file, "wb") as f:
    f.write(base64.b64decode(_fd_diff))
expect_red("forbid-deferral", ["python3", py("hygiene", "forbid_deferral.py"), "--diff-file", diff_file])

# 6b: #154 regression — a static check, not a live PR fetch. Both importers
# (pr-hygiene.py and hygiene/forbid_deferral.py) are Python, so both
# regression assertions run against the Python lib only.
#
# An inline "pr diff" subprocess call, whatever CLI-identity token it uses —
# a bare literal ("gh", or an operator's personal alias) or a variable name —
# is a duplicate of scripts/lib/gh_diff.py's fetchPrDiff() (#166 DRY); only
# gh_diff.py itself is allowed to make this call.
_INLINE_PR_DIFF_RE = re.compile(r'subprocess\.run\(\s*\[\s*("[^"]*"|\'[^\']*\'|[A-Za-z_][A-Za-z0-9_]*)\s*,\s*"pr"\s*,\s*"diff"')

_pr_hygiene_src = open(py("pr-hygiene.py"), "r", encoding="utf-8").read()
if _INLINE_PR_DIFF_RE.search(_pr_hygiene_src):
    print('SELFTEST FAIL: pr-hygiene.py still calls "pr diff" via its own inline subprocess instead of importing fetchPrDiff from lib.gh_diff (#166 DRY)', file=sys.stderr)
    failures += 1
elif not re.search(r"from\s+lib\.gh_diff\s+import\s+fetchPrDiff", _pr_hygiene_src):
    print("SELFTEST FAIL: pr-hygiene.py does not import fetchPrDiff from lib.gh_diff (#166 DRY)", file=sys.stderr)
    failures += 1
else:
    print("SELFTEST ok: pr-hygiene.py imports the shared fetchPrDiff() instead of duplicating the interactive-CLI call (#166 DRY)")

# gh_diff.py is the one file that IS allowed (expected) to make this call —
# but per #263, the CLI identity it invokes must be a configurable variable
# read once from the environment (defaulting sensibly), never a personal
# alias baked in as a string literal.
_gh_diff_py_src = open(py("lib", "gh_diff.py"), "r", encoding="utf-8").read()
_gh_diff_pr_diff_call = _INLINE_PR_DIFF_RE.search(_gh_diff_py_src)
if '"--patch"' in _gh_diff_py_src or "'--patch'" in _gh_diff_py_src:
    print("SELFTEST FAIL: lib/gh_diff.py still fetches the --patch (per-commit) diff (#154 regression)", file=sys.stderr)
    failures += 1
elif not _gh_diff_pr_diff_call:
    print('SELFTEST FAIL: lib/gh_diff.py — no "pr diff" invocation found; expected one (#154 regression check needs updating)', file=sys.stderr)
    failures += 1
elif _gh_diff_pr_diff_call.group(1).startswith('"') or _gh_diff_pr_diff_call.group(1).startswith("'"):
    print('SELFTEST FAIL: lib/gh_diff.py hardcodes its "pr diff" CLI identity as a string literal instead of a configurable variable (#263)', file=sys.stderr)
    failures += 1
else:
    _gh_diff_cli_var = _gh_diff_pr_diff_call.group(1)
    if not re.search(rf'{re.escape(_gh_diff_cli_var)}\s*=\s*os\.environ\.get\(', _gh_diff_py_src):
        print(f'SELFTEST FAIL: lib/gh_diff.py calls "pr diff" via {_gh_diff_cli_var}, but that name is never sourced from os.environ.get(...) — looks hardcoded rather than operator-configurable (#263)', file=sys.stderr)
        failures += 1
    else:
        print("SELFTEST ok: lib/gh_diff.py fetches the net PR diff, not --patch, via an environment-configurable CLI identity (#154, #263)")

_forbid_deferral_py_src = open(py("hygiene", "forbid_deferral.py"), "r", encoding="utf-8").read()
if _INLINE_PR_DIFF_RE.search(_forbid_deferral_py_src):
    print('SELFTEST FAIL: hygiene/forbid_deferral.py calls "pr diff" directly instead of importing fetchPrDiff from lib.gh_diff (#166 DRY)', file=sys.stderr)
    failures += 1
elif "fetchPrDiff" not in _forbid_deferral_py_src:
    print("SELFTEST FAIL: hygiene/forbid_deferral.py does not import fetchPrDiff from lib.gh_diff (#166 DRY)", file=sys.stderr)
    failures += 1
else:
    print("SELFTEST ok: hygiene/forbid_deferral.py imports the shared fetchPrDiff() instead of duplicating the interactive-CLI call (#166 DRY)")

# 6c: CONVENTIONAL_COMMITS_RE — #166 DRY. pr-hygiene.py and changelog.py
# both import the shared constant from lib/conventional_commits.py rather
# than defining their own copy.
for _file in ["pr-hygiene.py", "changelog.py"]:
    _src = open(py(_file), "r", encoding="utf-8").read()
    if re.search(r"CONVENTIONAL_COMMITS_RE\s*=\s*re\.compile", _src):
        print(f"SELFTEST FAIL: {_file} still defines CONVENTIONAL_COMMITS_RE locally instead of importing the shared constant (#166 DRY)", file=sys.stderr)
        failures += 1
    elif not re.search(r"from\s+lib\.conventional_commits\s+import\s+CONVENTIONAL_COMMITS_RE", _src):
        print(f"SELFTEST FAIL: {_file} does not import CONVENTIONAL_COMMITS_RE from lib.conventional_commits (#166 DRY)", file=sys.stderr)
        failures += 1
    else:
        print(f"SELFTEST ok: {_file} imports the shared CONVENTIONAL_COMMITS_RE instead of duplicating it (#166 DRY)")

# 7: pr-hygiene, fixture-based (a PR body that pastes the issue body verbatim).
expect_red("pr-hygiene", ["python3", py("pr-hygiene.py"), "--fixture", os.path.join(FIXTURES, "pr-hygiene")])

expect_green(
    "pr-hygiene (dependabot exemption, body rules only)",
    ["python3", py("pr-hygiene.py"), "--fixture", os.path.join(FIXTURES, "pr-hygiene-dependabot")],
)

out, err, code = run(["python3", py("pr-hygiene.py"), "--fixture", os.path.join(FIXTURES, "pr-hygiene-dependabot-review-required")])
_violation_lines = "\n".join(l for l in err.split("\n") if l.startswith("  "))
if code == 0:
    print("SELFTEST FAIL: pr-hygiene (dependabot PR with added human commit) was expected to fail on the missing review but exited 0", file=sys.stderr)
    failures += 1
elif "Article VIII.2" not in err:
    print(f"SELFTEST FAIL: pr-hygiene (dependabot PR with added human commit) failed, but not on the Article VIII.2 review gate:\n{out}{err}", file=sys.stderr)
    failures += 1
elif len([l for l in _violation_lines.split("\n") if l]) != 1:
    print(f"SELFTEST FAIL: pr-hygiene (dependabot PR with added human commit) reported more than just the review-gate violation — an Article III body rule leaked through the exemption:\n{_violation_lines}", file=sys.stderr)
    failures += 1
else:
    print("SELFTEST ok: pr-hygiene (dependabot PR with added human commit) still requires a Fresh-session review comment, and only that")

expect_red("pr-hygiene (no Fresh-session review comment)", ["python3", py("pr-hygiene.py"), "--fixture", os.path.join(FIXTURES, "pr-hygiene-review-missing")])
expect_red("pr-hygiene (Fresh-session review comment predates last commit)", ["python3", py("pr-hygiene.py"), "--fixture", os.path.join(FIXTURES, "pr-hygiene-review-stale")])
expect_green("pr-hygiene (Fresh-session review comment postdates last commit)", ["python3", py("pr-hygiene.py"), "--fixture", os.path.join(FIXTURES, "pr-hygiene-review-fresh")])

expect_green(
    "pr-hygiene (Fresh-session review via native PR review, not a comment)",
    ["python3", py("pr-hygiene.py"), "--fixture", os.path.join(FIXTURES, "pr-hygiene-review-native")],
)

for name in ["pr-hygiene-merged-before-review-112", "pr-hygiene-merged-before-review-125"]:
    out, err, code = run(["python3", py("pr-hygiene.py"), "--fixture", os.path.join(FIXTURES, name)])
    combined = out + err
    if code == 0:
        print(f"SELFTEST FAIL: pr-hygiene ({name}) was expected to fail on the merge-before-review audit but exited 0", file=sys.stderr)
        failures += 1
    elif not re.search(r"merged before review completed", combined):
        print(f"SELFTEST FAIL: pr-hygiene ({name}) failed, but not on the merge-before-review audit:\n{combined}", file=sys.stderr)
        failures += 1
    else:
        print(f"SELFTEST ok: pr-hygiene ({name}) correctly flags merging before its review completed")

expect_green("pr-hygiene (merged after review — no audit false-positive)", ["python3", py("pr-hygiene.py"), "--fixture", os.path.join(FIXTURES, "pr-hygiene-review-fresh")])

# check-pins: a submodule pinned to a branch (not its remote's default
# branch) that's freshly cloned + `submodule update --init`ed describes as
# "remotes/origin/<branch>", not "heads/<branch>".
tmp = tempfile.mkdtemp(prefix="cp-selftest-")

fake_duckdb = os.path.join(tmp, "fake-duckdb")
git_init(fake_duckdb, "trunk")
write_file(os.path.join(fake_duckdb, "f"), "x")
sh(fake_duckdb, "add f")
sh(fake_duckdb, "commit -q -m c")
sh(fake_duckdb, "tag v1.5.4")

fake_ci_tools = os.path.join(tmp, "fake-citools")
git_init(fake_ci_tools, "main")
write_file(os.path.join(fake_ci_tools, "f"), "main-content")
sh(fake_ci_tools, "add f")
sh(fake_ci_tools, "commit -q -m main-commit")
sh(fake_ci_tools, "checkout -q -b v1.5-variegata")
write_file(os.path.join(fake_ci_tools, "f"), "branch-content")
sh(fake_ci_tools, "add f")
sh(fake_ci_tools, "commit -q -m variegata-commit")
sh(fake_ci_tools, "checkout -q main")

super_dir = os.path.join(tmp, "super")
git_init(super_dir, "main")
sh(super_dir, f"submodule add -q -b trunk {fake_duckdb} duckdb")
sh(os.path.join(super_dir, "duckdb"), "checkout -q v1.5.4")
sh(super_dir, f"submodule add -q -b v1.5-variegata {fake_ci_tools} extension-ci-tools")
sh(super_dir, "add -A")
sh(super_dir, "commit -q -m add-submodules")

clone_dir = os.path.join(tmp, "super-clone")
sh(tmp, f"-c protocol.file.allow=always clone -q {super_dir} {clone_dir}")
sh(clone_dir, "submodule update --init")

expect_green("check-pins (detached-HEAD remote-tracking describe)", ["python3", py("check-pins.py"), "--root", clone_dir])

# 8-12: lanes-check's self-test fixtures.
for name in ["unregistered", "missing", "continue-on-error", "continue-on-error-step", "lanes-md-drift"]:
    expect_red(f"lanes-check ({name})", ["python3", py("lanes-check.py"), "--root", materialize(f"lanes-check-{name}")])

expect_red("docs-links (dead link)", ["python3", py("docs-links.py"), "--root", materialize("docs-links-dead-link")])
expect_red("docs-links (dead anchor)", ["python3", py("docs-links.py"), "--root", materialize("docs-links-dead-anchor")])

for name in ["bad-filename", "gap", "bad-status", "missing-date"]:
    expect_red(f"adr-lint ({name})", ["python3", py("adr-lint.py"), "--root", materialize(f"adr-lint-{name}")])

expect_red("fixtures-validate (invalid)", ["python3", py("fixtures-validate.py"), "--root", materialize("fixtures-validate-invalid")])
expect_red("fixtures-validate (inert)", ["python3", py("fixtures-validate.py"), "--root", materialize("fixtures-validate-inert")])

_dir = materialize("fixtures-validate-valid-provenance-token")
expect_green("fixtures-validate (valid, provenance token)", ["python3", py("fixtures-validate.py"), "--root", _dir])
expect_green("forbid-consumer (fixture provenance token exempt)", ["python3", py("hygiene", "forbid-consumer.py"), "--root", _dir])

expect_red("coverage-check", ["python3", py("coverage-check.py"), "--root", materialize("coverage-check-bad-how")])

expect_red("fixtures-validate (non-numeric window)", ["python3", py("fixtures-validate.py"), "--root", materialize("fixtures-validate-nonnumeric-window")])
expect_red("fixtures-validate (non-numeric lookback)", ["python3", py("fixtures-validate.py"), "--root", materialize("fixtures-validate-nonnumeric-lookback")])
expect_red("fixtures-validate (HISTOGRAM sample not a histogram literal)", ["python3", py("fixtures-validate.py"), "--root", materialize("fixtures-validate-histogram-bad-literal")])
expect_red("fixtures-validate (YAML-1.1 underscore-grouped int literal)", ["python3", py("fixtures-validate.py"), "--root", materialize("fixtures-validate-yaml11-underscore-int")])

expect_red("forbid-consumer (forbidden token in fixture: value)", ["python3", py("hygiene", "forbid-consumer.py"), "--root", materialize("forbid-consumer-fixture-value")])
expect_red("forbid-consumer (forbidden token in function: value)", ["python3", py("hygiene", "forbid-consumer.py"), "--root", materialize("forbid-consumer-function-value")])

# kernel-fixture-loader: #33.
expect_red("kernel-fixture-loader (REGRESSED: roster'd fixture now fails)", ["python3", py("hygiene", "kernel-fixture-loader.py"), "--root", materialize("kernel-fixture-loader-regressed")])
expect_red("kernel-fixture-loader (VANISHED: roster'd id has no current fixture)", ["python3", py("hygiene", "kernel-fixture-loader.py"), "--root", materialize("kernel-fixture-loader-vanished")])
expect_red("kernel-fixture-loader (ARRIVED-FAILING: new fixture, not in roster, fails)", ["python3", py("hygiene", "kernel-fixture-loader.py"), "--root", materialize("kernel-fixture-loader-arrived-failing")])
expect_red("kernel-fixture-loader (UNRECORDED: new fixture, not in roster, passes)", ["python3", py("hygiene", "kernel-fixture-loader.py"), "--root", materialize("kernel-fixture-loader-unrecorded")])
expect_green("kernel-fixture-loader (roster matches, everything passes)", ["python3", py("hygiene", "kernel-fixture-loader.py"), "--root", materialize("kernel-fixture-loader-green")])
expect_green("kernel-fixture-loader (reads test/fixtures/derived/**/*.yaml too)", ["python3", py("hygiene", "kernel-fixture-loader.py"), "--root", materialize("kernel-fixture-loader-derived-green")])

# derivation-sync: #37.
expect_red("derivation-sync (DROPPED: manifest names a fixture no file declares)", ["python3", py("hygiene", "derivation-sync.py"), "--root", materialize("derivation-sync-dropped")])
expect_red("derivation-sync (UNDECLARED: a file the manifest never named)", ["python3", py("hygiene", "derivation-sync.py"), "--root", materialize("derivation-sync-undeclared")])
expect_red("derivation-sync (UNRECORDED: manifest and files agree, roster doesn't)", ["python3", py("hygiene", "derivation-sync.py"), "--root", materialize("derivation-sync-unrecorded")])
expect_red("derivation-sync (VERSION-MISMATCH: derived_by doesn't match tool@tool_version)", ["python3", py("hygiene", "derivation-sync.py"), "--root", materialize("derivation-sync-version-mismatch")])
expect_green("derivation-sync (manifest, files and roster all agree)", ["python3", py("hygiene", "derivation-sync.py"), "--root", materialize("derivation-sync-green")])

# registry-roster-closure: #36.
expect_red(
    "registry-roster-closure (fixture-representable row, no fixture)",
    ["python3", py("hygiene", "registry-roster-closure.py"), "--root", materialize("registry-roster-closure-missing")],
)
expect_green(
    "registry-roster-closure (fixture present, ts_ prefix stripped)",
    ["python3", py("hygiene", "registry-roster-closure.py"), "--root", materialize("registry-roster-closure-green")],
)
expect_green(
    "registry-roster-closure (DOMAIN_NONE row needs no fixture)",
    ["python3", py("hygiene", "registry-roster-closure.py"), "--root", materialize("registry-roster-closure-domain-none-green")],
)

# divergence-enum-coverage: #36.
expect_red(
    "divergence-enum-coverage (unexercised member)",
    ["python3", py("hygiene", "divergence-enum-coverage.py"), "--root", materialize("divergence-enum-coverage-red")],
)
expect_green(
    "divergence-enum-coverage (every member exercised)",
    ["python3", py("hygiene", "divergence-enum-coverage.py"), "--root", materialize("divergence-enum-coverage-green")],
)

# tier-coverage-floor: #36.
for name, note in [
    ("tier-coverage-floor-regressed", "REGRESSED: coverage fell below the committed floor"),
    ("tier-coverage-floor-zero", "ZERO-FLOOR: a floor of 0 is refused outright"),
    ("tier-coverage-floor-untracked", "UNTRACKED: new coverage with no floor entry yet"),
    ("tier-coverage-floor-unknown-primitive", "UNKNOWN-PRIMITIVE: a citation names a primitive its Tier doesn't list"),
]:
    expect_red(f"tier-coverage-floor ({note})", ["python3", py("hygiene", "tier-coverage-floor.py"), "--root", materialize(name)])
expect_green(
    "tier-coverage-floor (coverage matches the committed floor exactly)",
    ["python3", py("hygiene", "tier-coverage-floor.py"), "--root", materialize("tier-coverage-floor-green")],
)

for name in ["missing-ref", "missing-extension", "missing-extension-name", "missing-repo", "missing-github", "maintainers-not-array"]:
    expect_red(
        f"description-validate ({name})",
        ["python3", py("description-validate.py"), "--file", os.path.join(FIXTURES, f"description-validate-{name}", "description.yml")],
    )

tmp = tempfile.mkdtemp(prefix="changelog-selftest-")
git_init(tmp, "main")
for subject in ["feat: add widget scalar (#1)", "fix: correct widget edge case (#2)", "docs: document the widget scalar (#3)"]:
    write_file(os.path.join(tmp, "f"), subject)
    sh(tmp, "add f")
    sh(tmp, f'commit -q -m "{subject}"')

run(["python3", py("changelog.py"), "--root", tmp])
with open(os.path.join(tmp, "CHANGELOG.md"), "r", encoding="utf-8") as f:
    generated = f.read()
if not generated.startswith("<!-- generated by make changelog -->") or "feat: add widget scalar (#1)" not in generated:
    print("SELFTEST FAIL: changelog did not write the expected entries", file=sys.stderr)
    print(generated, file=sys.stderr)
    failures += 1
else:
    print("SELFTEST ok: changelog writes every Conventional-Commit subject since the first commit (no tags)")

expect_green("changelog-check (matches what changelog would write)", ["python3", py("changelog.py"), "--check", "--root", tmp])

write_file(os.path.join(tmp, "CHANGELOG.md"), generated + "\nhand-edited\n")
expect_red("changelog-check (hand-edited CHANGELOG.md)", ["python3", py("changelog.py"), "--check", "--root", tmp])

# 13: pr-label / issue-label-check / docs-links pure-function unit
# assertions. Their pure functions (guarded behind `if __name__ == "__main__"`, so importing them
# never touches the network) are exercised in-process, exactly like
# scanDiffForDeferral above — importlib.import_module() resolves a
# hyphenated module name by string lookup (unlike the `import`/`from
# import` statements, which require a valid identifier and can't name a
# hyphenated file at all), so no importlib.util.spec_from_file_location
# machinery or renaming is needed for these three, module-collision-free
# siblings.
_pr_label = importlib.import_module("pr-label")
_issue_label_check = importlib.import_module("issue-label-check")
_docs_links = importlib.import_module("docs-links")


def assert_equal(label, actual, expected):
    global failures
    a = json.dumps(actual)
    e = json.dumps(expected)
    if a != e:
        print(f"SELFTEST FAIL: {label} — expected {e}, got {a}", file=sys.stderr)
        failures += 1
    else:
        print(f"SELFTEST ok: {label}")


assert_equal("closesIssueNumber: single match", _pr_label.closesIssueNumber("intro\n\nCloses #42\n\nmore text"), 42)
assert_equal("closesIssueNumber: case-insensitive", _pr_label.closesIssueNumber("closes #7"), 7)
assert_equal("closesIssueNumber: no match", _pr_label.closesIssueNumber("no link here"), None)
assert_equal("closesIssueNumber: ambiguous (two matches)", _pr_label.closesIssueNumber("Closes #1\n\nAlso closes #2"), None)
assert_equal("missingLabels: some missing", _pr_label.missingLabels(["size:S", "area:ci"], ["size:S"]), ["area:ci"])
assert_equal("missingLabels: none missing", _pr_label.missingLabels(["size:S"], ["size:S", "area:ci"]), [])
assert_equal("isMissingLabel: missing both", _issue_label_check.isMissingLabel([]), True)
assert_equal("isMissingLabel: missing area", _issue_label_check.isMissingLabel(["size:S"]), True)
assert_equal("isMissingLabel: has both", _issue_label_check.isMissingLabel(["size:S", "area:ci"]), False)
assert_equal("slugify: basic", _docs_links.slugify("The layer map"), "the-layer-map")
assert_equal("slugify: strips punctuation", _docs_links.slugify("Registry closure & the fixture format"), "registry-closure--the-fixture-format")
assert_equal(
    "headingSlugs: collects every heading",
    sorted(_docs_links.headingSlugs("# Title\n\nSome text\n\n## A section\n")),
    ["a-section", "title"],
)

# scanDiffForDeferral: pure-function assertions on actual violation
# content/count, run directly against hygiene/forbid_deferral.py's
# implementation (imported by its normal underscored module name at the
# top of this file, unlike the hyphenated pr-label/issue-label-check/
# docs-links imports above) — exercised in-process the same way as those.
with open(os.path.join(FIXTURES, "forbid-deferral.json"), "r", encoding="utf-8") as f:
    _manifest = json.load(f)


def _decode(key):
    return base64.b64decode(_manifest[key]).decode("utf-8")


assert_equal("scanDiffForDeferral: violation + genuine exemption, exact violations", scanDiffForDeferral(_decode("diff")), _manifest["diffExpected"])
assert_equal(
    "scanDiffForDeferral: comment-marker digits do not satisfy the #<issue> exemption",
    scanDiffForDeferral(_decode("wordBoundaryBypassDiff")),
    _manifest["wordBoundaryBypassExpected"],
)
assert_equal(
    "scanDiffForDeferral: deferral phrase split across a wrapped comment is still caught",
    scanDiffForDeferral(_decode("multiLineWrapDiff")),
    _manifest["multiLineWrapExpected"],
)
assert_equal(
    "scanDiffForDeferral: exemption reachable across the same wrapped comment block",
    scanDiffForDeferral(_decode("multiLineWrapExemptDiff")),
    _manifest["multiLineWrapExemptExpected"],
)
assert_equal(
    "scanDiffForDeferral: an unrelated earlier comment's #<issue> does not exempt the next comment's real violation",
    scanDiffForDeferral(_decode("adjacentUnrelatedCommentsDiff")),
    _manifest["adjacentUnrelatedCommentsExpected"],
)

# Now the real tree must be green: hygiene.py (#241) orchestrates
# lanes-check/docs-links/adr-lint/fixtures-validate/description-validate.py
# and the rest of the scans below.
out, err, code = run(["python3", py("hygiene.py")])
sys.stdout.write(out)
sys.stderr.write(err)
if code != 0:
    print("SELFTEST FAIL: `make hygiene` is not green on the real tree", file=sys.stderr)
    failures += 1
else:
    print("SELFTEST ok: `make hygiene` is green on the real tree")

for label, cmd in [
    ("lanes-check", ["python3", py("lanes-check.py")]),
    ("docs-links", ["python3", py("docs-links.py")]),
    ("adr-lint", ["python3", py("adr-lint.py")]),
    ("fixtures-validate", ["python3", py("fixtures-validate.py")]),
    ("kernel-fixture-loader", ["python3", py("hygiene", "kernel-fixture-loader.py")]),
    ("derivation-sync", ["python3", py("hygiene", "derivation-sync.py")]),
    ("registry-roster-closure", ["python3", py("hygiene", "registry-roster-closure.py")]),
    ("divergence-enum-coverage", ["python3", py("hygiene", "divergence-enum-coverage.py")]),
    ("tier-coverage-floor", ["python3", py("hygiene", "tier-coverage-floor.py")]),
    ("description-validate", ["python3", py("description-validate.py")]),
]:
    out, err, code = run(cmd)
    sys.stdout.write(out)
    sys.stderr.write(err)
    if code != 0:
        print(f"SELFTEST FAIL: `make {label}` is not green on the real tree", file=sys.stderr)
        failures += 1
    else:
        print(f"SELFTEST ok: `make {label}` is green on the real tree")

# Deliberately no "`make changelog-check` is green on the real tree"
# assertion here — CHANGELOG.md is only ever current relative to main's own
# tip, so an open branch can't guarantee it (Article II.2).

# check-pins: the "heads/<branch>" describe form.
tmp = tempfile.mkdtemp(prefix="cp-heads-selftest-")

fake_duckdb = os.path.join(tmp, "fake-duckdb")
git_init(fake_duckdb, "trunk")
write_file(os.path.join(fake_duckdb, "f"), "x")
sh(fake_duckdb, "add f")
sh(fake_duckdb, "commit -q -m c")
sh(fake_duckdb, "tag v1.5.4")

fake_ci_tools = os.path.join(tmp, "fake-citools")
git_init(fake_ci_tools, "main")
write_file(os.path.join(fake_ci_tools, "f"), "main-content")
sh(fake_ci_tools, "add f")
sh(fake_ci_tools, "commit -q -m main-commit")
sh(fake_ci_tools, "checkout -q -b v1.5-variegata")
write_file(os.path.join(fake_ci_tools, "f"), "branch-content")
sh(fake_ci_tools, "add f")
sh(fake_ci_tools, "commit -q -m variegata-commit")

super_dir = os.path.join(tmp, "super")
git_init(super_dir, "main")
sh(super_dir, f"submodule add -q -b trunk {fake_duckdb} duckdb")
sh(os.path.join(super_dir, "duckdb"), "checkout -q v1.5.4")
sh(super_dir, f"submodule add -q -b v1.5-variegata {fake_ci_tools} extension-ci-tools")
sh(super_dir, "add -A")
sh(super_dir, "commit -q -m add-submodules")

describe, _, _ = run(["git", "-C", super_dir, "submodule", "status"])
if "heads/v1.5-variegata" not in describe:
    print(f"SELFTEST FAIL: heads/<branch> fixture setup didn't reproduce the expected describe form: {describe}", file=sys.stderr)
    failures += 1

expect_green("check-pins (local heads/<branch> describe)", ["python3", py("check-pins.py"), "--root", super_dir])

# check-pins: #47 — the storage-partner pin.
tmp = tempfile.mkdtemp(prefix="cp-partner-selftest-")

fake_duckdb = os.path.join(tmp, "fake-duckdb")
git_init(fake_duckdb, "trunk")
write_file(os.path.join(fake_duckdb, "f"), "x")
sh(fake_duckdb, "add f")
sh(fake_duckdb, "commit -q -m c")
sh(fake_duckdb, "tag v1.5.4")

fake_ci_tools = os.path.join(tmp, "fake-citools")
git_init(fake_ci_tools, "main")
write_file(os.path.join(fake_ci_tools, "f"), "main-content")
sh(fake_ci_tools, "add f")
sh(fake_ci_tools, "commit -q -m main-commit")
sh(fake_ci_tools, "checkout -q -b v1.5-variegata")
write_file(os.path.join(fake_ci_tools, "f"), "branch-content")
sh(fake_ci_tools, "add f")
sh(fake_ci_tools, "commit -q -m variegata-commit")
sh(fake_ci_tools, "checkout -q main")

fake_rawduck = os.path.join(tmp, "fake-rawduck")
git_init(fake_rawduck, "main")
write_file(os.path.join(fake_rawduck, "f"), "rawduck-content")
sh(fake_rawduck, "add f")
sh(fake_rawduck, "commit -q -m rawduck-commit")
pinned_commit, _, _ = run(["git", "-C", fake_rawduck, "rev-parse", "HEAD"])
pinned_commit = pinned_commit.strip()

super_dir = os.path.join(tmp, "super")
git_init(super_dir, "main")
sh(super_dir, f"submodule add -q -b trunk {fake_duckdb} duckdb")
sh(os.path.join(super_dir, "duckdb"), "checkout -q v1.5.4")
sh(super_dir, f"submodule add -q -b v1.5-variegata {fake_ci_tools} extension-ci-tools")
write_file(
    os.path.join(super_dir, "scripts", "partners", "rawduck.json"),
    json.dumps({"repository": "example/fake-rawduck", "commit": pinned_commit, "duckdb_ref": "v1.5.4"}),
)
sh(super_dir, "add -A")
sh(super_dir, "commit -q -m add-submodules-and-partner-pin")

expect_green(
    "check-pins (partner pin present, build/partners/rawduck/ not built yet — pending, not fatal)",
    ["python3", py("check-pins.py"), "--root", super_dir],
)

partner_checkout = os.path.join(super_dir, "build", "partners", "rawduck")
sh(tmp, f"-c protocol.file.allow=always clone -q {fake_rawduck} {partner_checkout}")
expect_green("check-pins (partner pin matches build/partners/rawduck/ checkout)", ["python3", py("check-pins.py"), "--root", super_dir])

write_file(os.path.join(fake_rawduck, "f"), "rawduck-content-2")
sh(fake_rawduck, "add f")
sh(fake_rawduck, "commit -q -m rawduck-commit-2")
sh(partner_checkout, "pull -q origin main")
expect_red("check-pins (partner pin disagrees with build/partners/rawduck/ checkout)", ["python3", py("check-pins.py"), "--root", super_dir])

if failures > 0:
    print(f"hygiene-selftest: FAIL ({failures} check(s))", file=sys.stderr)
    sys.exit(1)
print("hygiene-selftest: PASS (every scan red on its fixtures, hygiene and lanes-check green on the tree)")
