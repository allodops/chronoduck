#!/usr/bin/env python3
"""make build-relevant-changed

Port of scripts/build-relevant-changed.mjs. Determines whether a PR's
changed files could affect the compiled extension -- writes
BUILD_RELEVANT=true/false to $GITHUB_ENV (or prints it, outside CI) so
ci.yml's build-test job can skip its expensive steps (ccache, `make
release`, `make test`, `make smoke`) on a docs/script-only PR instead of
running a full build unconditionally on every PR (#181 -- confirmed
wasteful in practice: #180 touched only docs/scripts/a fixture and still
ran the full build).
"""

import os
import sys
from pathlib import Path

from lib.git_base import changedFiles, resolveBase

# Anything that could change what `make release`/`make test`/`make smoke`
# actually build or exercise. `test/sql/` is here because `make test` runs
# every `.test` file in it; `test/hygiene-fixtures/` and other `test/`
# subdirectories are hygiene-only and deliberately excluded. `ci.yml` itself
# is here so a change to the build-relevance logic (or the workflow around
# it) always gets verified against a real build at least once.
BUILD_RELEVANT_PREFIXES = [
    "src/",
    "test/sql/",
    "CMakeLists.txt",
    "extension_config.cmake",
    "Makefile",
    "duckdb",
    "extension-ci-tools",
    ".github/workflows/ci.yml",
]


def is_build_relevant(path):
    return any(path == p or path.startswith(p) for p in BUILD_RELEVANT_PREFIXES)


def _root_from_args(args):
    if "--root" in args:
        return Path(args[args.index("--root") + 1])
    return Path.cwd()


def _base_from_args(args):
    if "--base" in args:
        return args[args.index("--base") + 1]
    return None


def main():
    args = sys.argv[1:]
    root = _root_from_args(args)
    explicit_base = _base_from_args(args)

    # Declared before either fail-open early-exit below so emit()'s
    # `len(files)` is always a real list (0, if a base/diff never resolved).
    files = []

    def emit(value):
        line = f"BUILD_RELEVANT={'true' if value else 'false'}"
        github_env = os.environ.get("GITHUB_ENV")
        if github_env:
            with open(github_env, "a", encoding="utf8") as f:
                f.write(line + "\n")
        print(f"build-relevant-changed: {line} ({len(files)} file(s) changed)")

    base = resolveBase(root, explicit_base)
    if not base:
        # Fail open toward running the build, not skipping it -- an unresolvable
        # base means "unknown," and a false "not relevant" is the unsafe wrong
        # answer here (a real code change silently skipping the build it needs).
        print(
            "build-relevant-changed: no base ref to diff against — treating as relevant (fail open toward building)",
            file=sys.stderr,
        )
        emit(True)
        sys.exit(0)

    try:
        files = changedFiles(root, base)
    except Exception:  # noqa: BLE001 - mirrors the .mjs's catch-all
        print(
            "build-relevant-changed: could not diff against base — treating as relevant (fail open toward building)",
            file=sys.stderr,
        )
        emit(True)
        sys.exit(0)

    relevant = any(is_build_relevant(f) for f in files)

    emit(relevant)


if __name__ == "__main__":
    main()
