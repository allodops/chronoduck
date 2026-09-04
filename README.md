# ChronoDuck

ChronoDuck is a DuckDB extension providing the building blocks a time-series query layer needs — grid-aligned range folds as combinable parallel aggregates, lookback resampling, and mergeable histogram state — exposed as plain SQL functions (`ts_*`) over any DuckDB relation. It carries a precise, counter-reset-aware default rate semantics with no options set, exposes the edge-mode and lookback alternatives other engines have converged on as named parameters, and binds to a caller's schema through a query-time profile rather than any storage-specific code path.

Time series increasingly land in DuckDB-readable form — Parquet lakes, DuckLake, schema-later
stores like RawDuck — and DuckDB has an unusually good set of relational primitives for time
(`ASOF JOIN`, `RANGE` frames, `time_bucket`, `FILL`). What it lacks is the semantics a time-series
query layer is built on: a precise `rate` over a fixed grid with lookback and staleness,
counter-reset handling, extrapolation to the window edges, native and exponential histogram
merges. Every project that wants those on a columnar engine today builds them inside its own
planner — GreptimeDB on DataFusion, Elastic in ES\|QL, ClickHouse's `timeSeries*ToGrid` family —
and none of them are reachable from DuckDB.

`ts_rate(start, end, step, window)(ts, value)` returns one row per series per grid point, computed
by a streaming per-series operator for range folds (memory proportional to the window, not the
query) and by combinable aggregates for instant and slice folds; both scale with cores. A schema
profile maps the kernel onto whatever tables the caller has. Downstream query layers are thin
table-function front-ends built separately, on top of a kernel that never learns their names.

The project's second deliverable is its test fence. The kernel's `combine` runs in an order the
engine does not promise, so the strongest claim it can make is not correctness against a reference
implementation alone but correctness *and* self-consistency under any partition, thread count,
vector size and row order. `docs/testing/` follows from that sentence: registered means fenced, one
derived tolerance, identity ratchets, no allow-lists, and a fence that is as language-unaware as the
code.

## Why it should exist

Three observations make the case.

First, the function shape has converged. ClickHouse, GreptimeDB, Elastic and the archived Promscale
all landed on a grid-parameterised per-series aggregate returning an array per series (see
`docs/prior-art.md`). That is strong evidence the API is right and that the novelty budget belongs
in state design and host integration, not in inventing a surface.

Second, no such thing exists for DuckDB. The community registry has statistical and forecasting
extensions, OTLP-to-Parquet pipelines, and various ingestion tooling, but no precise, grid-aligned
range functions; the closest users get today is a bucket-then-diff approximation, which is not what
a rate-based query was written against and which users notice.

Third, the hard parts have been solved before in ways that transfer. Sliding-window theory says
which folds are incremental and which must buffer; existing mergeable counter states and downsampling
designs show a precise, mergeable counter state is achievable; several engines' edge-mode selectors
show which edge modes are actually composable; DuckDB's own aggregate contract (state / update /
combine / finalize, variable-size state) is the right shape for all of it. ChronoDuck is a
synthesis, not an invention.

## Goals and non-goals

### Goals

Precise range, instant and histogram functions over any DuckDB relation, on a fixed evaluation
grid, with declared edge modes and per-profile lookback. Bit-deterministic output under any
parallel partition for every function whose state permits it, and a ratcheting budget for the rest.
Peak memory that follows a stated law per state class — proportional to the window per series for
streaming range folds, to the number of grid steps for slice folds, constant for instant folds —
asserted by sentinels, never assumed. A schema profile that binds at query time to a caller's own
tables, including a stale-marker role for writers that carry staleness as a flag rather than a NaN.
A SQL surface useful on its own, with no downstream query layer installed. A test suite that makes
a regression against any of the above a red check, not a review comment.

### Non-goals

Parsing any query language; speaking any wire format; owning ingestion or storage; cross-series
aggregation, label algebra, offset/`@`/subquery transforms, scalar math and date functions (all
DuckDB natives or consumer-side); a visualization front-end; matching any other engine's float
ordering to the last bit where a summation-reorder bound already proves equivalence.

## Principles

1. **Folds, edges, grids and value domains, not downstream consumers.** The registry, the
   fixtures, the tests and the docs describe those — a consumer's name may appear only in a
   provenance field or an appendix.
2. **SQL-native surface.** Inputs are ordinary columns; grouping is the caller's `GROUP BY`; the
   grid is a first-class table; results are `LIST`s aligned to it. A function a user cannot call
   from plain SQL does not belong in the kernel.
3. **Exactness modulo reorder, and nothing looser.** One comparator, derived from the summation
   backward-error bound, applied to every value in every layer. No fixture, function or lane can
   widen it.
4. **Determinism by construction.** States that buffer raw samples and sort before folding are
   bit-deterministic under any partition; that is the default state class, and the
   memory-cheaper slice state is an optimisation that must earn its way to the same class.
5. **Semantics are defaults, alternatives are parameters.** With no options set you get a precise,
   counter-reset-aware default. Anchored and smoothed edges, other tie-breaks, other lookbacks are
   named and opt-in, never silent.
6. **Storage-agnostic through a profile, never through code paths.** The kernel binds column roles
   at query time; it has no import for any specific storage system or wire format.
7. **The fence exists before the second function does.** Every registered function is present in
   every test layer or the build fails; no skip, tolerance or allow-list has a place to live.
