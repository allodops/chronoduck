<!-- scope: the fourteen enforced testing rules, each with its enforcement mechanism -->

# Rules

Each rule is enforced by a named mechanism, never by review. Where a rule's origin is prior art, it
is cited; the rest are new to this kernel.

1.  **T1. Registered means fenced.** A function exists only through the registry macro; the
    macro's expansion generates the dispatch entry *and* the required test-presence records. A
    registered function missing from any layer's case list is a compile or test failure named after
    the function. (Prior art: `nativeTSGridFn`-driven scan-bound tests; grid-carrier completeness.)
2.  **T2. No skip, no soft-assert, no tolerance file, no allow-list.** A `forbid-skip` scan rejects
    `GTEST_SKIP`, `DISABLED_` prefixes, `EXPECT_NO_FATAL_FAILURE`-only tests, empty sqllogictest
    expectations, and any `require` in a `.test` file other than the extension itself; because
    `require` skips silently, L13 additionally asserts that every `.test` file produced at least one
    query result in the run log. The single sanctioned exclusion is a closed-vocabulary *declared
    divergence* (see the comparator) with an issue link and an age cap. (Prior art: invariants 6, 7;
    `forbid-skip.mjs`.)
3.  **T3. One comparator, one derivation, applied to every value in every layer.** The derivation is
    parameterised by a conditioning quantity the fold exposes (Σ|terms|), so cancelling folds are
    held to the bound their arithmetic admits rather than excused from it. No fixture, function or
    lane can widen it. A headroom test pins the gap between the largest accepted ULP drift and the
    smallest real divergence ever observed. (Prior art: `summationReorderRelativeTolerance` and
    `TestEqualValuesRejectsRealDivergence`.)
4.  **T4. Every function declares a determinism class and is held to it.** `D0`: bit-identical
    output under any partition, thread count, vector size and row order. `D1`: within the reorder
    bound. `D1` is a budget that ratchets toward zero; a function may only move from `D1` to `D0`,
    never back.
5.  **T5. Oracles never import the kernel.** The from-scratch evaluator and the fixture harness live
    in a separate build target whose include path cannot reach the extension's sources; a meta-test
    walks includes and fails on any edge. (Prior art: `parity_oracle_imports_test.go`.)
6.  **T6. A test that is not provably executed does not count.** A meta-test parses CI workflow
    steps and the test binaries' registered names and refuses a test that no failure-propagating,
    unconditional step runs. A tagged suite without an executing lane fails closed. (Prior art:
    `tagged_test_enrollment_test.go`.)
7.  **T7. Ratchets gate on identity, never counts.** Every roster — fixture IDs, ShapeIDs,
    partition schemes, mutation legs — records the set that must pass; the verdicts `REGRESSED`,
    `VANISHED`, `ARRIVED-FAILING`, `UNRECORDED` are all fatal. A swap that leaves the count
    unchanged is caught. (Prior art: `compat-ratchet.mjs`.)
8.  **T8. Goldens are regenerated, never edited, and cannot certify what they cannot see.**
    Generated artefacts are `-merge` in `.gitattributes`; regeneration is sharded and refuses to run
    for a shard the diff does not imply. Any property that changes what is *read* but not what is
    *answered* (scan bounds, buffer caps, the operator's one-window residency) is asserted on the
    predicate or the counter, not on output. (Prior art: invariant 9;
    `range_window_grid_native_scan_bound_test.go`.)
9.  **T9. Memory is a tested contract.** Per-series state bytes and peak RSS on a 30-day,
    1 s-resolution query are asserted with numbers against a law per state class, and a self-check
    lane deliberately breaks each bounding mechanism to prove its sentinel fires. (Prior art:
    `cursor_chaos_test.go` 32 MiB bound; `perf-nightly-selfcheck.yml`.)
10. **T10. Sanitizers are lanes, not options.** ASan+UBSan (with `float-cast-overflow` and
    `float-divide-by-zero`) on the merge gate; TSan on every combine test, because a race in
    `combine` only exists under parallel aggregation.
11. **T11. Mutation efficacy is per translation unit, floored at 95%, and scored monotonically.**
    Unadjudicated timeouts sit in the denominator only; a closed status set; a measured per-mutant
    budget. Surviving relational-operator mutants in reset detection, window-edge and
    extrapolation-threshold code must be zero. (Prior art: `mutation-phases.mjs`,
    `gremlins-threshold.mjs`.)
12. **T12. Citations name constructs, not lines.** Every "NOT KILLABLE", "kills", and "pins" comment
    names `` file:function:`construct` `` and a gate resolves each to exactly one code line. (Prior
    art: `verify-code-citations.mjs`.)
13. **T13. Divergence from a reference is a bug, never a tweak.** The only permitted normalisations
    are those the kernel *documents as its contract* (NULL for "no value on this grid point" is the
    one candidate), listed in a closed enum the comparator reads — never an ad hoc adjustment tuned
    per query.
14. **T14. Testing is language-unaware.** No file under the kernel repository parses, names or
    depends on a query language. Fixtures are expressed in the kernel's own vocabulary (see the
    fixture format); provenance is a data field, never a code path. A `forbid-consumer` scan rejects
    query-language tokens in code and in fixture *keys*, and any import of a consumer's parser or
    engine anywhere in the repository, tests included. The forbidden token set is closed and
    committed in `scripts/hygiene/consumer-tokens.json` — the query-language names and every
    function name from each consumer's function table, generated from those tables and never
    hand-edited; oracle and reference-system names (Prometheus, ClickHouse, Timescale, Kusto,
    Graphite, OpenTSDB, InfluxDB, kdb+) are allowed, but a registry comment cites the algorithm or
    the paper, never a system's function (`Jugel 2014`, not a vendor's accessor). Exempt paths are
    listed in the same file: `CONSTITUTION.md`, `docs/prior-art.md`, `docs/decisions/`,
    `docs/design/ecosystem.md`, `docs/design/coverage.md` — the scan's declared holes, each named.
    Fixture *values* (provenance strings and pattern text) are exempt separately, by scan logic
    rather than a path-list entry: the scan reads fixture *keys*, never the provenance or
    pattern-text values a fixture carries. One embedded
    mini-language is admitted: a grammar that is a standard's (ISO/IEC 9075-2:2016 §R row-pattern
    syntax), whose parser is a Tier primitive with its own fence, and which appears only in fixture
    values; a consumer's grammar never is. The derivation tools that turn upstream corpora into
    fixtures are separate projects and are themselves consumers of the kernel.
