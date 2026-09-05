---
status: accepted
date: 2026-09-05
deciders: tsouza
---

# RawDuck as the first storage partner (L15)

## Context

`docs/testing/storage-partners.md` (new, this PR) defines L15's contract: a storage partner owns
physical layout, chronoduck owns computation over the rows a partner's tables produce, and the
integration surface is ordinary pushed filters, a profile, at most one view, and the
serialised-state contract — nothing partner-specific under `src/`. That contract needed a real
partner to build the harness against rather than an abstract description, and
[RawDuck](https://github.com/quackscience/rawduck) ("RawMergeTree-like Extension for DuckDB") is
the concrete match: it is a real, currently-maintained DuckDB extension whose own constructs are
exactly the shapes L15's integration surface names, not a hypothetical fit.

Reading RawDuck's actual source (pinned at `scripts/partners/rawduck.json`'s commit, fetched into
`build/partners/rawduck/` by `make partner-rawduck-build`) rather than assuming its shape from its
README turned up three constructs that ground each half of that contract in real code:

- A filter-observing optimizer hook. RawDuck registers a DuckDB `OptimizerExtension` whose
  `optimize_function` walks every optimized plan for filters DuckDB already pushed into base table
  scans and for `GROUP BY` columns, feeding a per-column usage cache. Citation (kept on one line so
  `scripts/hygiene/verify-citations.mjs`'s per-line match actually sees it — see this ADR's own
  Consequences section on why a citation into `build/partners/` must resolve on one line):
  `build/partners/rawduck/src/raw_optimize.cpp:RawDuckOptimizeHook:` `CollectStats(input.context, *plan, operators);`.
  This is L15's "ordinary pushed filters" from the partner's own side: chronoduck pushes nothing
  RawDuck-aware, and RawDuck observes only what any extension can observe through DuckDB's standard
  optimizer-extension API — the exact boundary the contract requires.
- `raw_optimize(table)`, a table function that turns that observed usage into a physical decision:
  `build/partners/rawduck/src/raw_optimize.cpp:GetRawOptimizeFunction:` `TableFunction("raw_optimize", {LogicalType::VARCHAR}, RawOptimizeFunction, RawOptimizeBind, RawOptimizeInit)`.
  It ranks columns by observed filter/group usage and physically reorders the table by the hottest
  two — incrementally, when the table only grew append-only since the last call. This is L15's "a
  profile" — a workload-driven decision made and applied entirely inside the partner's own storage,
  invisible to and unneeded by chronoduck.
- The `otlp-metrics` built-in transform:
  `build/partners/rawduck/src/raw_json.cpp:RawBuiltinTransforms:` `{"otlp-metrics", "resourceMetrics.scopeMetrics.metrics"}`.
  The layout it actually produces was read from the source rather than assumed, per this issue's own
  instruction: the transform explodes an OTLP/JSON metrics envelope at the dotted path
  `resourceMetrics.scopeMetrics.metrics` (one row per element of that array), then — because the
  transform is OTLP-flavored (`RawTransformIsOtlp`) — runs RawDuck's OTLP normalization over every
  resulting row before schema inference: `OtlpNormalizeObject` spreads each `attributes` `KeyValue`
  list into its parent object as plain sibling fields, and `OtlpNormalizeValue`/`OtlpUnwrapAnyValue`
  unwrap every OTLP `AnyValue` wrapper down to its bare scalar. RawDuck's ordinary schema inference
  then flattens whatever scalar fields survive normalization into typed columns — there is no
  metrics-specific column list hardcoded in the transform itself; the resulting shape is whatever a
  given export's `metrics[]` entries have after normalization, nothing more specific claimed here.

All three constructs exist under exactly the names this issue's Goal named them by
(`otlp-metrics`, `raw_optimize`, "the filter-observing optimizer hook") — no fabricated citation was
needed to satisfy that requirement.

A second finding, incidental to the search above but load-bearing for the harness's build step
(`scripts/partners/rawduck-build.mjs`): RawDuck maintains its own `v1.5.4` branch
(`quackscience/rawduck@7807eee995bbcbd8ace0ea7b8b61c2bd52862dfa`, "Bump DuckDB submodule and CI to
v1.5.4 (Variegata)."), whose own `duckdb` submodule gitlink
(`08e34c447bae34eaee3723cac61f2878b6bdf787`) is *already* chronoduck's own `duckdb` submodule pin —
the same tag `v1.5.4`, the same commit. `scripts/partners/rawduck.json` pins that commit for exactly
this reason: it is the point on RawDuck's own history where our two DuckDB pins already agree
without the build's own re-pointing step needing to move anything, which is the lowest-risk anchor
to build the very first version of this harness against. The re-pointing step exists regardless
(`scripts/lib/duckdb-pin.mjs`'s `EXPECTED_DUCKDB_REF`, not `rawduck.json`'s informational
`duckdb_ref`, is what the build actually checks out) — this commit choice does not make that
mechanism untested, since a bump to either repo's own DuckDB pin will separate the two again on the
very next `make partner-rawduck-build`, and the re-point is what re-converges them.

## Decision

RawDuck is adopted as the first partner under `docs/testing/storage-partners.md`'s L15 contract,
pinned in `scripts/partners/rawduck.json` at
`quackscience/rawduck@7807eee995bbcbd8ace0ea7b8b61c2bd52862dfa`. This ADR governs
`docs/testing/storage-partners.md`'s "The first partner: RawDuck" section; a future partner commit
bump, or a second partner, amends that doc directly and does not require a new ADR unless it changes
the L15 contract itself.

## Consequences

- `git grep -i rawduck src/` stays empty by construction: every RawDuck-specific fact this ADR
  records (the transform's explode path, the optimizer hook, `raw_optimize`'s existence) lives here,
  in `docs/testing/storage-partners.md`, and in `scripts/partners/`/`test/partners/` — never in the
  kernel.
- The citations above point into `build/partners/rawduck/`, a build artifact
  (`scripts/partners/rawduck-build.mjs`'s plain `git clone`, gitignored, never committed).
  `scripts/hygiene/verify-citations.mjs` checks a `build/partners/` citation strictly when that path
  exists on disk and skips it, not a violation, when it doesn't — the fast `hygiene` lane every PR
  runs never builds the partner, so these three citations get their real, strict check only in the
  `partner-rawduck` lane, which does build it first (`make verify-citations` is one of that lane's
  own steps). A rename inside RawDuck's own source that breaks one of these citations is a real
  signal that this ADR's Context has drifted from the partner's actual code, exactly as Article
  II.4 intends for any citation.
- If RawDuck ever renames or removes `raw_optimize`, `RawBuiltinTransforms`, or the
  `RawDuckOptimizeHook`/`optimize_function` wiring, `partner-rawduck` fails on the citation check
  before it fails on anything else, naming which construct moved — this ADR's Context is the record
  a maintainer reads to re-derive the citation rather than guess at it.
- Bumping `scripts/partners/rawduck.json`'s pinned commit in the future is expected to eventually
  separate the two DuckDB pins this ADR's Context notes currently agree; that is not a regression —
  it is `scripts/lib/duckdb-pin.mjs`'s re-pointing step doing the job it exists for, and a build
  failure at that point names the real incompatibility the harness exists to catch, per this issue's
  own acceptance criteria.
