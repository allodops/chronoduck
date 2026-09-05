#!/usr/bin/env python3
"""make release-checklist

Port of scripts/release-checklist.mjs. Prints the steps for cutting a
chronoduck release. Informational only -- no step here runs automatically;
each names the make target or manual action that carries it out.
"""

STEPS = [
    "1. Bump the duckdb / extension-ci-tools submodule pins to the versions this release targets, then `make check-pins` to confirm they agree with the workflow file.",
    "2. `make hygiene-selftest` and `make test` green on the release build (`make release` first).",
    "3. `make changelog` to refresh CHANGELOG.md, then `make changelog-check` to confirm it's exactly what a fresh generation would produce.",
    "4. Bump docs/community/description.yml's extension.version and repo.ref, then `make description-validate`. Set repo.ref to the current HEAD commit (the last substantive source commit) — NOT the eventual tag commit, whose SHA doesn't exist yet and can't be known before this edit. repo.ref will therefore trail the tag by exactly this one bump commit, which is correct: the bump commit itself carries no build-relevant source changes.",
    "5. Commit the description.yml/changelog bump from steps 3-4, then tag that new commit (`git tag vX.Y.Z && git push origin vX.Y.Z`) — this becomes the next `make changelog` run's range start.",
    "6. Open a PR to duckdb/community-extensions adding/updating docs/community/description.yml's contents under extensions/chronoduck/description.yml (tracked in #103, M11 · Release — this checklist only prints the step).",
]


def main():
    print("release-checklist:")
    for step in STEPS:
        print(f"  {step}")


if __name__ == "__main__":
    main()
