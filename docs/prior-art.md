<!-- scope: prior art — the sources this kernel's semantics and API shape are checked against -->

# Prior art

## What must the functions compute

Fifty-two sources, each fetched and checked, organised by the question each one answers. \[CORE\] marks the ones to read first; \[CAVEAT\] a limit or trap; \[NEXT\] source not yet read in depth.

> *Figure.* The one picture every source below is about. Prometheus's classic `rate` extrapolates to the window edges from the samples inside; the newer `anchored` and `smoothed` modes (and VictoriaMetrics) also read one sample before the window. That single difference decides whether a range function is decomposable across parallel partials — the central question for a DuckDB aggregate.

PromQL is the strongest consumer, so its function semantics are the kernel's default behaviour. These are the primary definitions.

- **[extrapolatedRate — prometheus/promql/functions.go](https://github.com/prometheus/prometheus/blob/main/promql/functions.go) \[CORE\]**
  *prometheus/prometheus · main · Go*
  ↳ The fold to reproduce: durations from first/last sample to the window edges, `extrapolationThreshold = avgInterval × 1.1`, half-interval extrapolation past it, the counter clamp at zero, and the final `factor`. The same file holds every `*_over_time` fold, `resets`, `changes`, `deriv`, `predict_linear`, `holt_winters` and the histogram quantile. Note the branch to `extendedRate` for anchored/smoothed — a second code path.
- **[Querying basics — staleness, lookback delta, left-open ranges](https://prometheus.io/docs/prometheus/latest/querying/basics/)**
  *prometheus.io · docs*
  ↳ Instant selectors take the newest sample within the lookback (default 5 m, overridable per query); range selectors are `(t − range, t]`; stale markers are a special NaN. Lookback and the staleness NaN are parameters of the kernel, not constants.
- **[Functions reference — histogram_quantile, \*\_over_time, resets, changes](https://prometheus.io/docs/prometheus/latest/querying/functions/)**
  *prometheus.io · docs*
  ↳ The edge cases: linear interpolation inside classic buckets with the lowest bucket assumed to start at 0, +Inf handling, *exponential* interpolation inside native-histogram buckets, equal sample weight in `*_over_time` regardless of spacing.
- **[Extended range selectors — anchored / smoothed](https://prometheus.io/docs/prometheus/latest/feature_flags/) · [Rabenstein, KubeCon EU 2026](https://hosted-files.sched.co/kccnceu2026/f8/2026-03-24%20KubeCon.pdf) \[CORE\]**
  *prometheus 3.13 · feature flag promql-extended-range-selectors · slides, Mar 2026*
  ↳ Anchored reads the last sample before the range; smoothed interpolates at both edges. Rabenstein's argument is that classic `rate` is not composable (`delta` over adjacent ranges ≠ delta over the union) while anchored/smoothed nearly are — precisely the property a parallel `combine` wants. Plan for three modes and a window fetch of `(t − range − lookback, t + lookahead]`.
- **[Native histograms specification](https://prometheus.io/docs/specs/native_histograms/) \[CORE\]**
  *prometheus.io · specs · stable since v3.8*
  ↳ Schemas −4…8 with bounds `(2^(2^−n))^i`, sparse spans, zero bucket, counter-reset hints, reset detection on any bucket drop or resolution reduction, merge at the lowest common schema. Defines the histogram struct and the merge kernel.
- **[promqltest scripts and testdata](https://github.com/prometheus/prometheus/blob/main/promql/promqltest/README.md)**
  *prometheus/prometheus · promql/promqltest/testdata/\*.test*
  ↳ Not a language-conformance suite here but a per-function oracle: `load` / `eval range from … to … step …` with native-histogram literals gives thousands of (samples, grid, expected values) triples that a kernel test harness can load into tables directly.
- **[VictoriaMetrics MetricsQL — documented deviations](https://docs.victoriametrics.com/victoriametrics/metricsql/) · [Valyala on irate](https://valyala.medium.com/why-irate-from-prometheus-doesnt-capture-spikes-45f9896d7832)**
  *VictoriaMetrics docs · 2019/2021*
  ↳ No extrapolation; reads the last raw sample before the window; omitted `[window]` defaults to `max(step, scrape_interval)`. Essentially Prometheus's anchored mode — a second reference for the `mode` parameter and evidence that consumers beyond PromQL want it.
- **[Understanding PromQL and its quirks](https://chronosphere.io/learn/understanding-promql-and-its-quirks/)**
  *Chronosphere, 2024*
  ↳ Lookback should be per-resolution (5 m raw, 3 h at 1 h downsampled) — a property of the table, not of the function.

## How does a window slide over a grid

Grid-aligned evaluation is sliding-window aggregation with the window jumping one `step` at a time. The stream-processing literature has already classified which aggregates can be done incrementally, in what order, and at what cost — and DuckDB's own windowing is built on the same theory.

- **[General Incremental Sliding-Window Aggregation](https://www.vldb.org/pvldb/vol8/p702-tangwongsan.pdf) \[CORE\]**
  *Tangwongsan, Hirzel, Schneider, Wu · PVLDB 8(7), 2015*
  ↳ The lift / combine / lower decomposition — the same three operations as DuckDB's update / combine / finalize. Read as a checklist it tells you which `*_over_time` functions are monoids and which (extrapolated rate, quantile) need the raw window.
- **[DABA / DABA Lite — worst-case constant-time in-order sliding windows](https://arxiv.org/abs/2009.13768)**
  *Tangwongsan, Hirzel, Schneider · VLDB Journal 30(6), 2021*
  ↳ O(1) insert/evict/query for FIFO windows without requiring invertibility, so `min/max_over_time` qualify. Applies to a per-series forward scan where the grid window slides monotonically.
- **[FiBA — out-of-order sliding-window aggregation](https://www.vldb.org/pvldb/vol12/p1167-tangwongsan.pdf) · [bulk evictions, PVLDB 2023](https://arxiv.org/abs/2307.11210)**
  *Tangwongsan, Hirzel, Schneider · PVLDB 12(10), 2019*
  ↳ Finger B-tree aggregator, O(log d) for out-of-order distance d. Relevant because morsel-parallel `combine` hands you samples from arbitrary time slices; the bulk-eviction follow-up is the "jump one whole step" operation.
- **[Scotty — efficient window aggregation with general stream slicing](https://www.dfki.de/en/web/research/projects-and-publications/publication/10263) · [Cutty, CIKM 2016](https://dblp.org/rec/conf/cikm/CarboneTKHM16.html)**
  *Traub et al. · EDBT 2019 / TODS 2021 · Carbone et al. · CIKM 2016*
  ↳ Slice the stream at window boundaries, aggregate each slice once, assemble windows from slices. A grid is the special case: slices are step-aligned buckets, and pre-bucketed slice partials are an *O(steps)* state for the functions where that is exact. Cutty covers several ranges over one scan.
- **[Sliding-Window Aggregation Algorithms (survey)](https://link.springer.com/referenceworkentry/10.1007/978-3-319-63962-8_157-1)**
  *Hirzel, Schneider, Tangwongsan · Encyclopedia of Big Data Technologies*
  ↳ Two-stacks, DABA, FlatFAT and FiBA side by side by invertibility, order and window shape. Use it to pick the state per function family.
- **[Efficient Processing of Window Functions in Analytical SQL Queries](https://www.vldb.org/pvldb/vol8/p1058-leis.pdf)**
  *Leis, Kundhikanjana, Kemper, Neumann · PVLDB 8(10), 2015*
  ↳ Segment trees over the partition give O(log n) arbitrary frames from associativity alone; DuckDB adopted this pipeline. It is what you get for free if a combinable aggregate is used under `RANGE BETWEEN … PRECEDING` — and why grid rows must exist in the input for that path to work.
- **[Windowing in DuckDB](https://duckdb.org/2021/10/13/windowing) · [Flying Through Windows (2025)](https://duckdb.org/2025/02/14/window-flying) · [Fast Moving Holistic Aggregates](https://duckdb.org/2021/11/12/moving-holistic) \[CORE\]**
  *Richard Wesley · DuckDB blog, 2021–2025*
  ↳ Segment-tree windowing built purely from update/combine/finalize, 1024-way hash-partitioned parallel sort, and the streaming-window path — the closest existing DuckDB mechanism to a per-series forward scan. The holistic post shows when a custom `window()` callback beats the segment tree (quantile, mode).
- **[DuckDB's AsOf Joins](https://duckdb.org/2023/09/15/asof-joins-fuzzy-temporal-lookups) · [Planning AsOf Joins](https://duckdb.org/2025/02/19/asof-plans) · [kdb+ aj](https://code.kx.com/q/ref/aj/)**
  *Richard Wesley · DuckDB blog, 2023/2025 · KX reference*
  ↳ Instant resampling with lookback is an ASOF JOIN of the grid onto samples with an inequality bound; the blog's cost model says when to reuse the operator versus write your own. kdb's `aj` is the origin of the semantics — note it has no staleness cutoff.
- **[FILL window function](https://github.com/duckdb/duckdb/discussions/17040) · [Stream windowing functions](https://duckdb.org/2025/05/02/stream-windowing-functions)**
  *duckdb/duckdb · shipped 2025 · DuckDB blog, 2025*
  ↳ Core's own answers to gap-filling and tumbling/hopping windows (`FILL`, `time_bucket`, `range` + join). Check before reimplementing; they cover the gauge cases but not lookback-with-staleness or counter semantics.
- **[Thanos ring buffer — ringbuffer/generic.go](https://github.com/thanos-io/promql-engine/blob/main/ringbuffer/generic.go)**
  *thanos-io/promql-engine · Go*
  ↳ For a sorted per-series stream, all grid steps are computed in one pass with two monotone pointers. Same trick as GreptimeDB's `RangeManipulate`; it is what a DuckDB aggregate's `finalize` does over a sorted `(t, v)` buffer.

## What state merges exactly

Systems that pre-aggregate metrics had to answer "what partial state is exact under merge" years ago. Their answers are the candidate state layouts for the DuckDB aggregates.

- **[TimescaleDB Toolkit counter_agg](https://github.com/timescale/timescaledb-toolkit/blob/main/docs/counter_agg.md) · [time_weight](https://www.tigerdata.com/docs/reference/toolkit/time_weight/time_weight) \[CORE\]**
  *Timescale · Rust / PostgreSQL extension · docs*
  ↳ A `CounterSummary` of first / second / penultimate / last points, reset-adjusted sums, reset count and regression sums; `rollup()` merges *non-overlapping* summaries and errors on overlap. The most directly reusable combinable rate/increase state, and it documents the ordering constraint `combine` must respect. `time_weight`'s prev/next-neighbour interpolation is the same problem as boundary extrapolation.
- **[Thanos downsampling — pkg/compact/downsample](https://github.com/thanos-io/thanos/blob/main/pkg/compact/downsample/downsample.go)**
  *thanos-io/thanos · Go · [compactor docs](https://thanos.io/tip/components/compact.md/)*
  ↳ AggrChunks keep count/sum/min/max plus a counter aggregate of first raw, reset-adjusted cumulative, and last raw values, downsampled only at chunk boundaries so the edges stay true. Concrete precedent for a Prometheus-exact mergeable counter state.
- **[Monarch: Google's planet-scale in-memory time series database](https://www.vldb.org/pvldb/vol13/p3181-adams.pdf)**
  *Adams et al. · PVLDB 13(12), 2020*
  ↳ An explicit `align` operator puts every series on a regular grid *before* any cross-series operation; cumulative points carry their start time so deltas survive gaps. Align-then-combine is the ordering rule, and the start-time idea maps onto OTel's `StartTimeUnix`.
- **[Mimir query sharding](https://grafana.com/docs/mimir/latest/references/architecture/query-sharding/) · [Thanos distributed execution proposal](https://thanos.io/tip/proposals-done/202301-distributed-query-execution.md/)**
  *Grafana / Thanos docs*
  ↳ Read purely as a combinability list: `sum/min/max/count/avg` shard; `absent*`, `histogram_quantile`, `sort*`, `topk` do not; `sum(rate(m))` becomes N inner rates plus an outer sum. The same list says which functions can be DuckDB partial aggregates and which are finalize-only.
- **[ClickHouse -State / -Merge combinators](https://clickhouse.com/docs/sql-reference/aggregate-functions/combinators)**
  *ClickHouse docs*
  ↳ The user-facing pattern for exposing partial states as storable columns — `rate_state(…)` / `rate_merge(…)` — which a downsampling table needs later.
- **[BTrDB](https://www.usenix.org/conference/fast16/technical-sessions/presentation/andersen) · [Scuba](https://vldb.org/pvldb/vol6/p1057-wiener.pdf) · [Procella](https://www.vldb.org/pvldb/vol12/p2022-chattopadhyay.pdf) · [M3 aggregator](https://m3db.io/docs/architecture/m3aggregator/)**
  *FAST 2016 · PVLDB 2013 · PVLDB 2019 · M3 docs*
  ↳ Four systems whose core move is "everything is a combinable partial": tree-stored min/max/mean/count, aggregation trees of sketches, SQL-defined rollups at compaction, multi-stage streaming rollups. Evidence that rate and histogram states should be storable and re-mergeable, not just transient.
- **[Morsel-Driven Parallelism](https://db.in.tum.de/~leis/papers/morsels.pdf) · [Parallel Grouped Aggregation in DuckDB](https://duckdb.org/2022/03/07/aggregate-hashtable) \[CORE\]**
  *Leis, Boncz, Kemper, Neumann · SIGMOD 2014 · DuckDB blog, 2022*
  ↳ Why `combine` sees arbitrary, non-contiguous time slices of one series: thread-local pre-aggregation then radix-partitioned merge. The state therefore has to carry boundary samples, and the merge has to sort.
- **[InfluxDB 3.0's FDAP architecture](https://www.influxdata.com/blog/flight-datafusion-arrow-parquet-fdap-architecture-influxdb/) · [QuestDB SAMPLE BY](https://questdb.com/blog/the-story-of-our-sample-by-enhancements/)**
  *Andrew Lamb · InfluxData, 2023 · QuestDB, 2024*
  ↳ Time series on a general engine via custom operators (dedup, gap-fill) under several front-ends — the closest architectural analogue. QuestDB's `SAMPLE BY … ALIGN TO CALENDAR … FILL(PREV|LINEAR)` is a worked-out SQL surface for fixed grids, useful when naming the extension's functions.

## How do histograms merge

- **[OpenTelemetry metrics data model — ExponentialHistogram](https://opentelemetry.io/docs/specs/otel/metrics/data-model/) \[CORE\]**
  *OTel specification*
  ↳ `base = 2^(2^−scale)`, perfect subsetting so downscaling is lossless, zero bucket, dense offset+counts. One merge kernel (downscale to the lower scale, reconcile the zero threshold) serves OTel histograms and Prometheus native histograms — modulo the off-by-one index convention.
- **[How sparse histograms improve efficiency, precision and mergeability](https://grafana.com/blog/2021/11/03/how-sparse-histograms-can-improve-efficiency-precision-and-mergeability-in-prometheus-tsdb/)**
  *Plaetinck, Vernekar · Grafana Labs, 2021*
  ↳ The design rationale behind native histograms: mergeability across schemas is the property. Any histogram aggregate must stay associative under schema downgrade.
- **[Mergeable Summaries](https://cse.hkust.edu.hk/~yike/pods12-mergeable.pdf)**
  *Agarwal, Cormode, Huang, Phillips, Wei, Yi · PODS 2012*
  ↳ The formal definition of "mergeable" — commutative, associative merge preserving size and error bounds. The theoretical footing for parallel `combine` of sketch-valued aggregates.
- **[DDSketch](https://arxiv.org/abs/1908.10693) · [UDDSketch](https://arxiv.org/abs/2004.08604) · [t-digest](https://arxiv.org/abs/1902.04023)**
  *Masson, Rim, Lee · PVLDB 2019 · Cafaro et al., 2020 · Dunning, Ertl, 2019*
  ↳ Log-spaced buckets with relative-error guarantees — the same geometry as exponential histograms; UDDSketch's bucket collapsing is the same operation as schema downscaling. t-digest backs DuckDB's own `approx_quantile` and is the fallback for `quantile_over_time` when no buckets exist.

## Who has built this kernel before

Four engines put grid-aligned range functions inside a general-purpose columnar planner in the last eighteen months. Their function signatures converge, which is the strongest available evidence for the API shape.

- **[ClickHouse PR 80590 — helper functions for PromQL-like queries](https://github.com/ClickHouse/ClickHouse/pull/80590) · [PR 86010 — changes/resets](https://github.com/ClickHouse/ClickHouse/pull/86010) · [timeSeriesRateToGrid](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/timeSeriesRateToGrid) \[CORE\]**
  *davenger, stephchi0 · merged Jun / Sep 2025 · C++*
  ↳ The closest thing to a reference implementation of this extension: parametric aggregates `timeSeriesRateToGrid(start, end, step, staleness)(ts, value) → Array(Nullable(Float64))`, with `ResampleToGridWithStaleness`, `Delta`, `InstantRate/Delta`, `Changes`, `Resets`, and `LastTwoSamples` for materialised views. Duplicates at a timestamp keep the max; "implementations directly mirror Prometheus algorithms". Read the C++ for the state layout and the combine.
- **[GreptimeDB promql crate on DataFusion](https://greptimedb.rs/promql/all.html) · [range_manipulate.rs](https://greptimedb.rs/src/promql/extension_plan/range_manipulate.rs.html) · [normalize.rs](https://greptimedb.rs/src/promql/extension_plan/normalize.rs.html) \[CORE\]**
  *GreptimeTeam · Rust · Apache-2.0*
  ↳ The plan-level version of the same kernel: `SeriesNormalize` (offset, stale-NaN filtering), `InstantManipulate` (grid + lookback), `RangeManipulate` (binary-search `(ts − range, ts]` into zero-copy `RangeArray` views), then `extrapolate_rate` and the `*_over_time` UDFs over those views. A DuckDB port collapses `RangeManipulate` + UDF into one grid-parameterised aggregate.
- **[Elastic ES\|QL TS source, RATE and TBUCKET](https://www.elastic.co/docs/reference/query-languages/esql/commands/ts) · [launch blog, Apr 2026](https://www.elastic.co/observability-labs/blog/elasticsearch-supports-promql)**
  *Elastic · ES\|QL · 2026*
  ↳ `STATS SUM(RATE(m, 5m)) BY TBUCKET(1m), host` — temporal aggregation per series, then spatial; `RATE` is a first-class aggregate any ES\|QL user can call. The clearest example of "designed for PromQL, not exclusive to it"; borrow the `buckets`/auto-step and `scrape_interval` parameters.
- **[Mimir Query Engine (MQE) — pkg/streamingpromql](https://github.com/grafana/mimir/blob/main/pkg/streamingpromql/README.md) · [design blog, Sep 2025](https://grafana.com/blog/faster-more-memory-efficient-performance-in-grafana-mimir-a-closer-look-at-mimir-query-engine/)**
  *Grafana Labs · Go · default since Mimir 3.0*
  ↳ `SeriesMetadata()` up front, then `NextSeries()` one series at a time; peak memory one input plus one output series (−92 % on `sum by` over 100 k series). That is DuckDB's hash-aggregate shape exactly; MQE's range-vector operators are the per-series fold the aggregate's `finalize` performs.
- **[Thanos promql-engine](https://github.com/thanos-io/promql-engine)**
  *thanos-io · Go · Volcano model*
  ↳ Step-vectors (all series at one timestamp) — the opposite orientation from MQE's series-at-a-time. Both map to DuckDB (GROUP BY series with a grid array, or GROUP BY step); worth benchmarking both before fixing the output shape.
- **[cerberus — docs/engine.md](https://github.com/tsouza/cerberus/blob/main/docs/engine.md)**
  *tsouza/cerberus · Go · Apache-2.0*
  ↳ The `RangeWindow` family shared by three consumers (PromQL, LogQL metric queries, TraceQL metrics), the native-grid capability nodes, the `Recollapse` eligibility list of functions exact under merged `-State`s, and the scan time-bound contract. Also a warning: native grid aggregates differ from Go's float ordering in the last bit.
- **[Promscale extension (archived 2024)](https://github.com/timescale/promscale_extension) \[CAVEAT\]**
  *Timescale · Rust / PostgreSQL*
  ↳ `prom_rate`, `prom_delta`, `prom_increase` as PostgreSQL aggregates — the earliest full attempt at this design. Its discontinuation argues for keeping the extension self-contained and schema-agnostic rather than coupled to a bespoke layout.

## What does DuckDB permit

What the engine does for you, what the aggregate contract actually looks like, and which doors are C++-only.

### Engine model

- **[Push-Based Execution in DuckDB](https://duckdb.org/library/push-based-execution-in-duckdb/) · [CMU 15-721 lecture](https://15721.courses.cs.cmu.edu/spring2023/slides/22-duckdb.pdf) · [internals overview](https://duckdb.org/docs/current/internals/overview.html)**
  *Raasveldt · DSDSD 2021 · CMU 2023 · docs*
  ↳ Source / Operator / Sink, pipelines, global and local state — the reason aggregate `combine` exists and the contract a custom physical operator satisfies if an aggregate turns out not to be enough.
- **[Vectors and the LIST layout](https://duckdb.org/docs/current/internals/vector.html)**
  *DuckDB docs*
  ↳ `list_entry_t{offset, length}` plus a child vector — what `finalize` writes when returning a grid as a LIST.
- **[External aggregation](https://duckdb.org/2024/03/29/external-aggregation) \[CAVEAT\]**
  *Kuiper · DuckDB blog, 2024*
  ↳ Aggregate states can spill, but heap memory hanging off a pointer in the state is not buffer-managed. Large per-series sample buffers need an arena or tracked allocation and a `destructor`; better still, bound the scan so the buffer stays small.
- **[Indexes: zone maps and ART](https://duckdb.org/docs/current/sql/indexes.html) · [Optimizers: the low-key MVP](https://duckdb.org/2024/11/14/optimizers) · [Fastest Table Sort in the West](https://duckdb.org/2021/08/27/external-sorting)**
  *DuckDB docs and blog, 2021–2024*
  ↳ Zone maps are the only pruning you get, so physical ordering by metric/series/time is the whole story; the optimizer post shows what already runs before and after an `OptimizerExtension`; the sort post matters if the state must sort in `finalize`.

### Aggregate contract

- **[core_functions/aggregate README](https://github.com/duckdb/duckdb/blob/main/extension/core_functions/aggregate/README.md) · [aggregate_function.hpp](https://raw.githubusercontent.com/duckdb/duckdb/main/src/include/duckdb/function/aggregate_function.hpp) \[CORE\]**
  *duckdb/duckdb · main*
  ↳ The callback set: size / initialize / update / combine / finalize / destructor / window / bind / statistics / serialize. Variable-size state via pointers is explicitly legal; `combine` must not mutate its source unless `ALLOW_DESTRUCTIVE`; `window()` is optional and "should be used sparingly". A correct `combine` buys parallel GROUP BY, spilling and segment-tree windowing at once.
- **[quantile.cpp — holistic aggregate with list output](https://raw.githubusercontent.com/duckdb/duckdb/main/extension/core_functions/aggregate/holistic/quantile.cpp) \[CORE\]**
  *duckdb/duckdb · extension/core_functions*
  ↳ The in-tree template for this exact shape: variable-size buffered state, `ListCombineFunction`, `StateDestroy`, a `window` over `SubFrames`, bind data carrying constant parameters (the quantile list — for you, the grid), and `finalize` writing one `list_entry_t` per group.
- **[Discussion 3605 — custom aggregate function](https://github.com/duckdb/duckdb/discussions/3605)**
  *duckdb/duckdb · pdet, hawkfish · 2022*
  ↳ Maintainer guidance: define `combine` and windowing comes free; write `window` only for large or holistic states.
- **[anofox-statistics](https://github.com/DataZooDE/anofox-statistics) · [quackstats](https://github.com/jasadams/quackstats)**
  *DataZooDE · C++ shell over Rust · jasadams · Rust via C API*
  ↳ Two existing statistical-aggregate extensions: one shows the C++-shell-over-Rust-numerics hybrid; the other shows what a pure-Rust time-series extension can and cannot do (no pushdown, no window, no operators).

### Beyond aggregates

- **[table_function.hpp](https://raw.githubusercontent.com/duckdb/duckdb/main/src/include/duckdb/function/table_function.hpp) · [Issue 19818 — pushdown caveat](https://github.com/duckdb/duckdb/issues/19818) \[CAVEAT\]**
  *duckdb/duckdb · main · Nov 2025*
  ↳ `bind_replace` lets a table function (`ts_grid`, or a later language front-end) return a sub-plan; with `filter_pushdown=true` there is no partial pushdown — unhandled predicates are dropped, not left in-plan.
- **[optimizer_extension.hpp](https://raw.githubusercontent.com/duckdb/duckdb/main/src/include/duckdb/optimizer/optimizer_extension.hpp) · [logical_extension_operator.hpp](https://raw.githubusercontent.com/duckdb/duckdb/main/src/include/duckdb/planner/operator/logical_extension_operator.hpp) · [physical_operator.hpp](https://raw.githubusercontent.com/duckdb/duckdb/main/src/include/duckdb/execution/physical_operator.hpp)**
  *duckdb/duckdb · main*
  ↳ The path to a real custom operator if the align/resample step needs one: optimizer hook → `LogicalExtensionOperator::CreatePlan` → a `PhysicalOperator` implementing Sink / Operator / Source.
- **[duckdb-vss](https://duckdb.org/2024/05/03/vector-similarity-search-vss) · [DuckPGQ (PVLDB 2023)](https://ir.cwi.nl/pub/33317/33317.pdf)**
  *Gabrielsson, 2024 · ten Wolde, Szárnyas, Boncz · CWI*
  ↳ vss is the smallest official example of an optimizer rule rewriting into a custom physical operator; DuckPGQ is the most honest published account of the extension API's pain points.
- **[C API reference](https://duckdb.org/docs/current/clients/c/api.html) · [extension-template-rs](https://github.com/duckdb/extension-template-rs) · [quack-rs](https://quack-rs.com/) · [Discussion 21159](https://github.com/duckdb/duckdb/discussions/21159) \[CAVEAT\]**
  *DuckDB docs · duckdb org · community · Mar 2026, unanswered*
  ↳ The C API (and therefore Rust) covers scalar, aggregate and table functions and projection pushdown — not filter pushdown, window callbacks, optimizer hooks or custom operators. Enough for the aggregate family alone; not for a plan-level align operator. C++ core with Rust numerics behind it is the hybrid.
- **[extension-template (C++)](https://github.com/duckdb/extension-template) · [community extension submission](https://duckdb.org/community_extensions/development.html)**
  *duckdb org · docs*
  ↳ Build, sqllogictest, cross-platform CI and signing; binaries are pinned to an exact DuckDB version, which sets the release cadence to track.

## Source to mine next

Published work says what the kernel should be. The reference implementations say what it actually computes per sample and per step — and the second and third consumers show where the fold differs when the input is a span duration or a log line rather than a metric sample.

- **[prometheus/promql/engine.go](https://github.com/prometheus/prometheus/blob/main/promql/engine.go) \[NEXT\]**
  *range-vector iteration, matrixIterSlice, buffer reuse between steps*
  ↳ How the per-series window buffer is populated and reused as the grid advances; the lookback and staleness handling on instant selectors; the point at which float and histogram samples diverge.
- **[prometheus/model/histogram/float_histogram.go](https://github.com/prometheus/prometheus/blob/main/model/histogram/float_histogram.go) \[NEXT\]**
  *Add, Sub, Compact, DetectReset, schema reduction*
  ↳ The merge, subtraction and reset-detection operations the histogram state must reproduce, including how a schema downgrade is applied during `Add`.
- **[grafana/tempo/pkg/traceql/engine_metrics.go](https://github.com/grafana/tempo/blob/main/pkg/traceql/engine_metrics.go) \[NEXT\]**
  *rate, count_over_time, quantile_over_time, histogram_over_time over spans · AGPL: read, don't link*
  ↳ A second consumer of the same kernel where the value is a span duration and the "sample" is a span start time. Where its `rate` differs from Prometheus's (no counters, no extrapolation) tells you which mode parameters the kernel needs.
- **[grafana/loki/pkg/logql/range_vector.go](https://github.com/grafana/loki/blob/main/pkg/logql/range_vector.go) \[NEXT\]**
  *step-window iteration over log samples · AGPL: read, don't link*
  ↳ A third consumer: `rate`, `count_over_time`, `bytes_rate` and the `unwrap` aggregations over log lines, with Loki's own step iteration. The overlap with Prometheus's folds is what the language-neutral surface must cover; the differences are the options.
- **[ClickHouse src/AggregateFunctions — AggregateFunctionTimeSeries\*](https://github.com/ClickHouse/ClickHouse/tree/master/src/AggregateFunctions) \[NEXT\]**
  *C++ · the ToGrid family's state, merge and serialisation*
  ↳ The one existing C++ implementation of exactly this aggregate shape; read for how state is laid out, how partial buffers are merged and sorted, and how the grid parameters are bound.

## What it adds up to

1.  **The function shape has converged.** ClickHouse `*ToGrid`, GreptimeDB `RangeManipulate`, ES\|QL `RATE … BY TBUCKET` and Promscale `prom_rate` all land on a grid-parameterised per-series aggregate returning one array per series. Adopt it; the novelty budget goes into state design and the host integration.
2.  **Extrapolated `rate` is not decomposable** across partial windows (Rabenstein 2026; the Mimir/Thanos combinability lists). The aggregate state is the raw window — or first/last/n/reset-accumulator, which *is* mergeable when partials are time-ordered — and `finalize` runs `extrapolatedRate`. Only outer `sum/min/max/count` layers are freely combinable.
3.  **Three rate modes** — classic, anchored, smoothed — exist as named alternatives, because Prometheus 3.x, VictoriaMetrics and ES\|QL each land on a variant, and the anchored one is the composable one. The window fetch includes one sample before the range, optionally one after.
4.  **Sliding-window theory decides the state per family.** Monoid functions (`sum/count/min/max_over_time`) get slice-based O(steps) state (Scotty); order-sensitive ones (rate, resets, changes, deriv, quantile) buffer samples and sort in `finalize`; out-of-order arrival from morsels is the normal case, not the exception (FiBA).
5.  **One histogram kernel serves both worlds.** Prometheus native and OTel exponential histograms share bucket geometry; merge is downscale-to-min-schema plus zero-threshold reconciliation, and quantile interpolation is exponential inside buckets. Timescale's `counter_agg` and Thanos downsampling are the precedents for storing these states.
6.  **Lookback is a table property, staleness is a value.** Lookback varies per resolution and belongs to the schema profile; the stale marker rides in the DOUBLE as NaN so nothing downstream has to special-case NULL.
7.  **`quantile.cpp` is the template.** It is the in-tree template for a variable-state, list-returning, windowable aggregate, and a correct `combine` buys parallel grouping, spilling and segment-tree windowing without a custom `window()`.
8.  **C++ for the plan-level pieces.** The C API lacks filter pushdown, window callbacks, optimizer hooks and custom operators (confirmed through Mar 2026). The aggregate family alone would fit Rust; an align/resample operator would not.
9.  **The reference implementations are the ongoing research.** `functions.go`, `engine.go` and `float_histogram.go` for what is computed; Tempo's `engine_metrics.go` and Loki's `range_vector.go` for where the second and third consumers diverge; ClickHouse's `AggregateFunctionTimeSeries*` for how it has already been laid out in C++.
