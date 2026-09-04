#!/usr/bin/env bun
// make release-checklist
// Prints the steps for cutting a chronoduck release. Informational only —
// no step here runs automatically; each names the make target or manual
// action that carries it out.
const STEPS = [
  "1. Bump the duckdb / extension-ci-tools submodule pins to the versions this release targets, then `make check-pins` to confirm they agree with the workflow file.",
  '2. `make hygiene-selftest` and `make test` green on the release build (`make release` first).',
  "3. `make changelog` to refresh CHANGELOG.md, then `make changelog-check` to confirm it's exactly what a fresh generation would produce.",
  "4. Bump docs/community/description.yml's extension.version and repo.ref to the commit being tagged, then `make description-validate`.",
  "5. Tag the release commit (`git tag vX.Y.Z && git push origin vX.Y.Z`) — this becomes the next `make changelog` run's range start.",
  "6. Open a PR to duckdb/community-extensions adding/updating docs/community/description.yml's contents under extensions/chronoduck/description.yml (Out of scope for M0 — tracked separately, this checklist only prints the step).",
];

console.log("release-checklist:");
for (const step of STEPS) console.log(`  ${step}`);
