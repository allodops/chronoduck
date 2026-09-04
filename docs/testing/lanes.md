<!-- scope: the merge/release/nightly gate posture model, the lane registry, and today's registered lanes -->

# Gates and lanes

Every check the project runs belongs to exactly one of three postures. The merge gate is a small, fixed set chosen for speed: it runs in minutes and does not vary with the diff. It consists of build, lint, forbid-skip, forbid-consumer, and, once the kernel exists, the fast correctness layers — L0 through L3 in unit mode, L4, L5, L7, L13, and ASan+UBSan — together with L6a's in-process chDB leg, L8, TSan on L4, and the coverage-enrollment half of the coverage lane. The release gate runs on push to main and on release branches, and no-ops on ordinary pull requests: the L6 provenance refresh, L6a's Timescale leg, L11 sentinels inside L14's public-surface run, the L12 provenance floor, the measured coverage floor, and a full mutation matrix. Nightlies run coverage-guided fuzzing, the full Mull matrix, and the memory self-check; their red is a bug to fix, never a tolerated state.

A lane registry (following cerberus's own `ci-lanes.json` naming) declares each lane's layers, oracle class, posture, build flags and owning workflow job, and a meta-test binds the registry to the actual workflow YAML — a lane whose step is disabled, wrapped in a continue-on-error escape hatch, or missing failure propagation is not a lane. A structural floor requires, for the kernel, at least one executing lane of each oracle class: execution, property, reference, mutation, coverage, resilience. Renaming is fine; deleting one fails validation.

Coverage is per translation unit, raise-only, and refuses to record zero; a drop must be a hand-edited line in the diff. The roster files — fixture IDs by provenance, ShapeIDs, partition schemes, MR IDs, fixture families, mutation legs — are all identity ratchets with the four fatal verdicts, and all gate the merge posture.

## The registry today

Chronoduck's own lane registry is the repository-relative path `.github/ci-lanes.json`; `make lanes-check` verifies it against the actual workflow files and against this document — a registered lane missing from this page, or a name on this page that isn't registered, both fail the check. M0 has no kernel yet, so today's lanes are the scaffolding and governance layer the kernel's own lanes above will join as they're built, not a replacement for that composition:

- **merge posture**: `hygiene` (the tree hygiene scans, L13), `build-test` (the release build and sqllogictest suite, L5), `relassert` (the UBSan/forced-assert build, L9), `duckdb-stable-build` and `code-quality-check` (the vendored distribution and format/tidy pipeline)
- **nightly posture**: `tsan` (ThreadSanitizer, L4/L9), `vector-size-2` (the reduced-vector-size debug build, L4)
- **release posture**: `analyze` (CodeQL), `scorecard` (OpenSSF Scorecard), `label` and `backfill` (the PR auto-labeler), `check` (the issue-label drift check)
