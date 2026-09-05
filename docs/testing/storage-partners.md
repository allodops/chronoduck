<!-- scope: L15 — storage partners: what the layer requires of a partner extension, the harness that builds and tests one, and the pin/cache/citation mechanics that make it reproducible -->

# Storage partners (L15)

A storage partner is a separately-maintained DuckDB extension that owns physical storage — table
layout, on-disk format, its own optimizer hooks over that layout — while chronoduck owns only
computation over the rows a partner's tables produce. The partner stores, chronoduck computes; the
integration surface is ordinary pushed filters, a profile, at most one view, and the
serialised-state contract. Nothing partner-specific lives under `src/`: `git grep -i rawduck src/`
staying empty is not a style preference, it's the whole point of the boundary — chronoduck's kernel
never learns a partner's name, table shape, or storage format. Everything partner-specific lives
under `scripts/partners/`, `test/partners/` and the build-time-only `build/partners/`.

This is deliberately a narrow contract. It does not mean "chronoduck understands RawMergeTree's
on-disk format" or "chronoduck pushes a RawDuck-specific optimizer rule" — it means a partner's
table is, from chronoduck's side, just a table: chronoduck's own registered functions (`ts_rate`,
`ts_grid`, …) run against it exactly as they run against a native DuckDB table or a Parquet scan,
through the same pushed filters, the same at-most-one-view surface, and the same serialised
aggregate state a partition-and-determinism run already exercises at L4. A partner earns L15
coverage by existing at all and building at chronoduck's own DuckDB pin — nothing more.

## The first partner: RawDuck

[RawDuck](https://github.com/quackscience/rawduck) ("RawMergeTree-like Extension for DuckDB") is
the first storage partner. It gives schema-on-write JSON tables a RawMergeTree-inspired physical
layout, driven by exactly the constructs this layer's integration surface names:

- **Pushed filters, observed.** RawDuck registers a DuckDB `OptimizerExtension` whose
  `optimize_function` hook walks every optimized plan for filters DuckDB has already pushed into a
  base table scan (`LogicalGet::table_filters`) and for `GROUP BY` columns, recording per-column
  usage counts it later uses to decide physical column order —
  `build/partners/rawduck/src/raw_optimize.cpp:RawDuckOptimizeHook:` `CollectStats(input.context, *plan, operators);`.
  This is the "ordinary pushed filters" half of L15's integration surface from RawDuck's own side:
  chronoduck pushes nothing partner-aware, and RawDuck observes only what any extension can observe
  through DuckDB's standard optimizer-extension API.
- **A profile, materialized as physical reordering.** `raw_optimize(table)` is a plain table
  function — `build/partners/rawduck/src/raw_optimize.cpp:GetRawOptimizeFunction:` `TableFunction("raw_optimize", {LogicalType::VARCHAR}, RawOptimizeFunction, RawOptimizeBind, RawOptimizeInit)`
  — that ranks a table's columns by observed filter/group usage and rewrites the table sorted by
  the hottest two, incrementally when the table only grew since the last call. This is the "a
  profile" half of the integration surface: a workload-driven decision entirely inside RawDuck's
  own storage, invisible to chronoduck.
- **The layout its `otlp-metrics` transform produces.** RawDuck's built-in transform table —
  `build/partners/rawduck/src/raw_json.cpp:RawBuiltinTransforms:` `{"otlp-metrics", "resourceMetrics.scopeMetrics.metrics"}`
  — explodes an OTLP/JSON metrics export envelope at the dotted path
  `resourceMetrics.scopeMetrics.metrics`, producing one row per element
  of that array (one row per metric definition, nested under its resource and scope). Because the
  transform is OTLP-flavored (`RawTransformIsOtlp`), the explode pass also runs RawDuck's OTLP
  value normalization over every resulting row before schema inference: `OtlpNormalizeObject`
  spreads each `attributes` `KeyValue` list directly into its parent object (so `attributes: [{key:
  "k", value: {stringValue: "v"}}]` becomes a plain sibling field `k: "v"`, real fields winning over
  a same-named attribute), and `OtlpNormalizeValue`/`OtlpUnwrapAnyValue` unwrap every OTLP
  `AnyValue` wrapper (`stringValue`/`intValue`/`doubleValue`/`boolValue`/`bytesValue`) down to its
  bare scalar. RawDuck's ordinary schema inference (`RawPayload::InferSchema` / `FlattenSchema`,
  the same machinery every other transform and ad-hoc JSON payload uses) then flattens whatever
  scalar fields survive normalization in each exploded row into typed columns — there is no
  metrics-specific column list hardcoded anywhere in the transform; the layout is exactly whatever
  structure a given OTLP metrics export's `metrics[]` entries happen to have after normalization.
  This is recorded here because it was read from the source, not assumed: see
  `docs/decisions/0019-rawduck-first-storage-partner.md` for why that distinction matters to this
  ADR.

None of this is chronoduck's concern at query time. What chronoduck's own harness (below) checks is
narrower still: that a RawDuck-backed table is queryable through chronoduck's registered functions
at all, at the DuckDB version chronoduck itself is pinned to.

## The harness

- `scripts/partners/rawduck.json` pins the partner: `repository`, the exact `commit` this repo
  builds against, and `duckdb_ref` — informational only, recording what that commit's *own*
  `duckdb` submodule pointed at when the commit was chosen. The build never reads `duckdb_ref`: it
  always re-points the partner's `duckdb` submodule at chronoduck's own pinned DuckDB tag
  (`scripts/lib/duckdb_pin.py`), because L15's whole premise — chronoduck's functions running
  unmodified over a partner's tables — requires both extensions to be built against the identical
  DuckDB version. A partner commit whose own pin has drifted from ours, and which fails to build
  once re-pointed, is exactly the incompatibility this harness exists to surface, not paper over.
- `make partner-rawduck-build` (`scripts/partners/rawduck-build.py`) fetches the pinned commit into
  `build/partners/rawduck/` — a plain `git clone`, never a submodule of this repo and never
  committed (`build/` is gitignored) — re-points its `duckdb` submodule as above, and builds
  `rawduck.duckdb_extension` with this repo's own `CMAKE_BUILD_PARALLEL_LEVEL` convention. The
  artifact is cached under `build/partners/rawduck-cache/<key>/`, keyed by a hash of the partner
  commit and chronoduck's own DuckDB pin — the only two inputs that can change what the build
  produces — so a repeat run with both unchanged copies the cached artifact instead of rebuilding.
- `make partner-rawduck-test` (`scripts/partners/rawduck-test.py`) LOADs chronoduck's own built
  extension and the freshly built `rawduck.duckdb_extension` into the same stock DuckDB shell and
  runs every `test/partners/rawduck/*.sql` file (deliberately not `*.test`: DuckDB's own sqllogictest
  runner auto-discovers `.test` files under `test/` regardless of directory, and a plain SQL script
  isn't valid sqllogictest syntax). For this layer's current scope that is a
  smoke-LOAD only: both extensions load without conflict, and a minimal RawDuck-backed table
  answers a `ts_rate` query. Real fixture-driven layout-parity testing against RawDuck's actual
  on-disk layout is out of scope here (T2.10).
- `make check-pins` reports the pinned partner commit alongside the `duckdb`/`extension-ci-tools`
  submodule pins, and fails when `scripts/partners/rawduck.json`'s commit disagrees with what's
  actually checked out at `build/partners/rawduck/` — skipped, not failed, when that directory
  doesn't exist yet (a hygiene-only run that never built the partner has nothing to compare
  against), matching this repo's established pattern for a check whose subject isn't always
  checked out (the root `Makefile`'s own extension-ci-tools-submodule-absent warning). This check
  is scoped to the pinned checkout only: it never looks at `build/partners/rawduck-head/` (below).
- The lane is registered as `partner-rawduck` in `.github/ci-lanes.json`, posture `release`
  (`.github/workflows/release.yml`, push to `main`, plus a `workflow_dispatch` escape hatch for
  verifying a fix from a branch before it reaches `main` — issue #225) — layer L15, owner tsouza.

## Tracking partner HEAD drift

`scripts/partners/rawduck.json`'s `commit` is a deliberate pin: `partner-rawduck` only ever proves
that *that exact* RawDuck commit still builds and loads against chronoduck's DuckDB pin. It says
nothing about whether RawDuck's own default branch has since moved somewhere incompatible — the
nightly `partner-rawduck-head` lane (`.github/workflows/nightly.yml`, posture `nightly`, layer L15,
owner tsouza) exists to catch exactly that, before it silently breaks compatibility for whoever
next bumps the pin.

`partner-rawduck-head` runs the identical two targets `partner-rawduck` runs —
`make partner-rawduck-build` and `make partner-rawduck-test` — with `RAWDUCK_REF=head` set. That
env var makes `scripts/partners/rawduck-build.py` resolve `scripts/partners/rawduck.json`'s
`repository` field's own default-branch tip at run time (`git ls-remote <repo> HEAD`, never
`rawduck.json`'s pinned `commit`), still re-points the resulting checkout's `duckdb` submodule at
chronoduck's own pin exactly as the pinned build does, and builds and caches it under its own
`build/partners/rawduck-head/` checkout and `build/partners/rawduck-head-cache/` cache root —
distinct from `partner-rawduck`'s paths, so the two lanes never disturb each other's checkout or
cache and `make check-pins`'s pinned-commit comparison keeps comparing against the pin, not
whatever HEAD happened to build most recently. `scripts/partners/rawduck-test.py`, under the same
env var, targets that HEAD checkout and reports the actual commit under test before running
`test/partners/rawduck/*.sql` against it — so the lane's own output names both the HEAD commit and,
on a red run, the failing leg (the `.sql` file whose script errored).

A red `partner-rawduck-head` run is a compatibility bug in the pin, not a chronoduck regression: the
fix is a partner-pin bump PR — bumping `scripts/partners/rawduck.json`'s `commit` to (or past) the
commit that broke, plus a `docs/decisions/0019-rawduck-first-storage-partner.md` amendment if the
new commit changed the layout the ADR records — filed from a `finding`-labeled issue the next work
loop opens against the failing run.

### Citations into a build artifact

The ADR below cites RawDuck's real source under `build/partners/rawduck/`, which is a build
artifact, not part of this tree in a plain checkout. `scripts/hygiene/verify-citations.py` checks
a citation whose path starts with `build/partners/` strictly (existence and exact-one-occurrence,
exactly like any other citation) when that path exists on disk, and skips it — not a violation —
when it doesn't, since the partner simply hasn't been built in that environment. The
`partner-rawduck` lane, which does build the partner before it runs, invokes `make verify-citations`
as one of its own steps, so these citations get a real, strict check on every push to `main` —
just not on the fast, submodule-independent `hygiene` lane every PR runs.
