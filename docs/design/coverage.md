<!-- scope: coverage matrix — every time-series primitive across surveyed systems mapped to a registry row, a DuckDB native, a consumer composition, the storage layer, or explicitly out of the query layer -->

# Coverage matrix

Dispositions: **K** existing registry row · **K+** row or option added during the surface-completeness review · **D** DuckDB native · **C** consumer composition of K and D · **S** storage layer · **P** planned family, not yet in the registry · **X** out of the query layer.

## 1. Time-range scan and pruning

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Range select on (t1, t2] with value predicate | TSMS survey; TSBS high-cpu; TSM-Bench Q1/Q2 | D | WHERE on native/Parquet/DuckLake scans; zone maps |
| Scan bound derived from grid, range and lookback | ChronoDuck brief | K | scan_bounds + the optimizer rule pushing an ordinary filter to the Get (T2.3); the two-gate rows-scanned law |
| Segment / zone-map pruning on series key | BTrDB; Gorilla; RawDuck raw_optimize | S | storage layout; RawDuck observes the pushed filter (T2.10) |

## 2. Bucketing and calendar alignment

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Fixed-width bucket floor (time_bucket, TIME_FLOOR, SAMPLE BY, date_histogram fixed) | all systems | D | time_bucket(width, ts, origin|offset) |
| Calendar buckets with timezone and DST (month/year, ALIGN TO CALENDAR, calendar_interval) | QuestDB; Druid; Timescale; Elastic; Datadog calendar rollup; OpenTSDB 1dc | D | time_bucket(width, ts, timezone) via ICU; the convention the contract relies on (sub-day widths uniform in UTC, day and coarser aligned to local midnight, 23/25-hour days) is pinned by an L5 fixture on a DST-crossing day |
| Regular grid (start, end, step) as a relation | ClickHouse timeSeriesRange; Kusto make-series | K | grid table function; grid_index scalar |
| Folds over calendar (non-uniform) buckets, e.g. monthly increase of a counter | Timescale counter_agg with_bounds; Datadog calendar rollup | K+ | bounded aggregate form: edge-reading folds (RAW_WINDOW, LAST_K, HIST_WINDOW rows) accept explicit (window_start, window_end) per group; INSIDE SLICE folds over calendar buckets are GROUP BY time_bucket (D); pushdown only when the bounds are constants |
| Bucket alignment to first observation / epoch / query start | QuestDB ALIGN TO FIRST OBSERVATION; Datadog epoch alignment | C | grid start is the consumer's choice; time_bucket origin |
| Adaptive bucket count (auto_date_histogram, Datadog default interval) | Elastic; Datadog | C | consumer picks step from range ÷ target points |

## 3. Cross-series aggregation

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| sum/min/max/avg/count/stddev/quantile/topk/count_values across series at a grid point | PromQL, MetricsQL, LogQL, Graphite, every SQL TSDB | D | GROUP BY over DuckDB aggregates on fold output (kernel boundary: 'deliberately not') |
| Cross-series histogram merge | PromQL sum over native histograms; MetricsQL histogram | K | hist_merge (HIST_MERGE, D1 float counts) |
| topk/bottomk re-evaluated per grid point | TraceQL topk; MetricsQL topk_min/max/avg/last | D | QUALIFY row_number() OVER (PARTITION BY t ORDER BY v) |
| Group-wise outliers (outliersk, outliers_mad, DBSCAN outliers) | MetricsQL; Datadog outliers | C | MAD/quantile over the group per grid point; DBSCAN is P (class 18) |
| count_nonzero / count_not_null across groups | Datadog | D | count(*) FILTER |
| Jain fairness index, weighted average across series | M3QL jainCP; Graphite weightedAverage | D | arithmetic over group aggregates |

## 4. Last point, first point, selection

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Last point per series (lastpoint, LATEST ON, last-loc) | TSBS; QuestDB; Druid LATEST_BY | D | arg_max(v, t) / QUALIFY; or resample at a single grid point |
| Last point per composite key | QuestDB LATEST ON ts PARTITION BY a, b | D | QUALIFY row_number() OVER (PARTITION BY a, b ORDER BY t DESC) = 1 |
| first/last by timestamp inside a window | PromQL first_over_time/last_over_time; Timescale first/last | K | first_over_time, last_over_time (SLICE) |
| Timestamps of min/max/first/last | PromQL ts_of_*; MetricsQL tmin/tmax/tfirst/tlast | K | ts_of_min/max/first/last_over_time |
| Timestamp of the last change | MetricsQL tlast_change_over_time | K+ | ts_of_last_change_over_time (RAW_WINDOW) |
| Last N points per series | CQL [Partition By … Rows N]; TSBS groupby-orderby-limit | D | QUALIFY row_number() ≤ N; last_two_samples for N=2 rollups |
| timestamp(v), start_timestamp(v), lag (age of last sample) | PromQL; MetricsQL lag | C | resample_ts; t − resample_ts |
| lifetime, scrape_interval (median inter-sample gap) | MetricsQL | K+ | interval_over_time (median gap, RAW_WINDOW); lifetime = ts_of_last − ts_of_first |

## 5. Counter semantics

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| rate / increase with extrapolation to the window edges | PromQL; ClickHouse timeSeriesRateToGrid; ES|QL RATE | K | rate, increase, EXTRAPOLATE mode; extrapolate() as a pure function of the fold summary |
| rate / increase using the last sample before the window, no extrapolation | MetricsQL rate/increase/delta; Timescale counter_agg delta | K | ANCHOR edge mode (edge_context anchor at or before t−w) |
| rate / increase with interpolated edges | Timescale interpolated_rate/delta; PromQL extendedRate | K | SMOOTH edge mode via extended_fold |
| irate / idelta from the last two samples | PromQL; MetricsQL; ES|QL IRATE/IDELTA; ClickHouse InstantRate | K | irate, idelta (LAST_K(2)) |
| resets count; changes count | PromQL; MetricsQL; ClickHouse ResetsToGrid/ChangesToGrid | K | resets, changes |
| Start-timestamp aware resets (created timestamps) | Prometheus isStartTimestampReset | K | two-part reset predicate (value drop ∨ st_reset); [t,v,st] samples |
| Counter wrap at a ceiling (maxValue, counterMax) | Graphite nonNegativeDerivative(maxValue); OpenTSDB counterMax | K+ | reset_policy = WRAP(counter_max, counter_min = 0, inclusive) — Graphite adds 1 (inclusive range), OpenTSDB does not; the flag is a parameter, never inferred |
| Reset suppression / drop (resetValue, dropResets, non_negative_*) | OpenTSDB; InfluxQL NON_NEGATIVE_DERIVATIVE; Datadog monotonic_diff | K+ | reset_policy = DROP (OpenTSDB dropResets) or NULL (Graphite: the point is None); reset_value (a rate above the ceiling is emitted as 0; for increase the rate × window is compared) |
| increase_pure (counter assumed to start at zero) | MetricsQL | K+ | zero_start option: the no-start-timestamp spelling of the single-sample rule; when a start timestamp is present it wins |
| remove_resets (monotonic rewrite of a series) | MetricsQL | C | running sum over the sample stream of (delta if delta ≥ 0 else cur) — a reset restarts from zero, it does not contribute 0 |
| deltaSum / deltaSumTimestamp (negative deltas ignored, mergeable with timestamp order) | ClickHouse | K+ | increase _state with reset_policy = ZERO (contributes 0 at a reset); deltaSumTimestamp is the ordered-merge rule counter_fold's _state already has |
| Per-second rate of counts, bytes, spans | LogQL rate/bytes_rate; TraceQL rate | C | count_over_time / range; sum_over_time / range ('registering arithmetic') |
| rate of a gauge treated as counter (LogQL rate_counter) | LogQL | K | rate over the unwrapped value with COUNTER domain |
| derivative without time division (Graphite derivative, Datadog diff) | Graphite; Datadog; InfluxQL DIFFERENCE | D | lag() window function on the sample stream |
| Elapsed time between points; lag-N difference | InfluxQL ELAPSED; Datadog dt(); Elastic serial_diff(lag=N) | D | t − lag(t, N), v − lag(v, N) on the sample stream |

## 6. Gauge folds over a window

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| count/sum/avg/min/max_over_time | everywhere | K | SLICE rows |
| stddev/stdvar_over_time (population) | PromQL; MetricsQL; LogQL; ES|QL | K | RAW_WINDOW two-pass |
| quantile_over_time, quantiles (many φ), median | PromQL; MetricsQL quantiles_over_time; TraceQL; LogQL | K | quantile_over_time; multi-φ is the multi-fold surface (K+, class 12) |
| Quantile estimation rule (Hyndman–Fan R-3 vs R-7) | OpenTSDB ep*r3 / ep*r7; Prometheus (R-7 linear) | K+ | quantile_over_time(φ, method R7|R3); R-7 is the default and the only convention the oracles share |
| mad_over_time | PromQL (experimental); MetricsQL | K | mad_over_time |
| range_over_time / SPREAD (max − min) | MetricsQL; InfluxQL SPREAD | K+ | range_over_time (SLICE, from MinMax partial) |
| sum2_over_time, geomean_over_time | MetricsQL | K+ | sum2_over_time (SLICE, D1), geomean_over_time (SLICE over log, D1) |
| mode_over_time, distinct_over_time, count_values_over_time | MetricsQL; Influx MODE | K+ | mode_over_time and distinct_over_time (RAW_WINDOW, exact); count_values is GROUP BY v then count_over_time (C) |
| zscore_over_time (last vs window mean/stddev) | MetricsQL | C | (last_over_time − avg_over_time) / stddev_over_time |
| hoeffding_bound_lower/upper | MetricsQL | C | from avg, range and count folds |
| stats bundle in one pass (min, argmin, max, argmax, avg, stdev, variance) | Kusto series_stats; Timescale stats_agg | K+ | multi-fold surface (class 12) returns a STRUCT in one pass |
| skewness, kurtosis over a window | Timescale stats_agg; Flux skew | K+ | moments_over_time STRUCT (RAW_WINDOW, RESIDUAL_SS); also needed by ASAP |
| present_over_time / absent_over_time | PromQL; MetricsQL; LogQL; ES|QL | K / C | present_over_time; absent = LEFT JOIN against grid |
| count_distinct_over_time (exact and approximate) | ES|QL COUNT_DISTINCT_OVER_TIME; ClickHouse uniq* | K+ | distinct_over_time exact (class 6) and approx_distinct_over_time HLL (class 19) |

## 7. Predicate and threshold folds

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| count_eq/ne/gt/le_over_time, sum_gt/le/eq_over_time | MetricsQL | C | WHERE on the value before a SLICE fold (edges do not move for INSIDE folds) |
| share_gt/le/eq_over_time | MetricsQL | C | numerator count_over_time on the filtered rows, denominator on all rows: two operator passes joined on (series, t), or one ts_fold with a FILTER member (K+) |
| Threshold filters on fold output (TraceQL > 1s, Graphite removeAbove/Below, Datadog cutoff) | TraceQL; Graphite; Datadog | D | WHERE / CASE on the output |
| removeAbovePercentile / range_trim_* (percentile of the whole range) | Graphite; MetricsQL | C | quantile over the output relation then filter |

## 8. Shape and monotonicity folds

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| increases_over_time / decreases_over_time | MetricsQL | K+ | monotone_over_time(kind) covering increases, decreases, ascent, descent (RAW_WINDOW) |
| ascent_over_time / descent_over_time (elevation gain/loss) | MetricsQL | K+ | same row, kind = ascent|descent |
| changed (1 where value differs from previous) | Graphite changed; M3QL | D | lag() comparison |
| running_sum/avg/min/max, cumsum, integral (running) | MetricsQL running_*; Datadog cumsum; Graphite integral; InfluxQL CUMULATIVE_SUM | D | cumulative window frames on fold output |
| integralByInterval (running sum resetting per bucket) | Graphite | D | window frame PARTITION BY time_bucket |

## 9. Instant value, gap-fill, interpolation

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Value at a grid point with lookback (staleness horizon) | PromQL instant selector; ClickHouse timeSeriesResampleToGridWithStaleness; MetricsQL default_rollup | K | resample (LOOKBACK), resample_ts |
| Stale markers end the carry | Prometheus staleness; MetricsQL stale_samples_over_time | K | stale_marker, is_stale; 'until the next non-stale sample' |
| Count of stale markers in a window | MetricsQL stale_samples_over_time | K+ | stale_samples_over_time (SLICE) |
| LOCF / keep_last_value / fill(previous) / locf() | Timescale; MetricsQL; Graphite keepLastValue; InfluxQL; Kusto fill_forward | K | resample LOOKBACK with lookback = ∞ or a bound; DuckDB last_value IGNORE NULLS covers rows-with-NULL |
| NOCB / keep_next_value / fill_backward | MetricsQL; Kusto; Graphite | K+ | resample mode NEXT (next − t ≤ lookback; a stale marker as the next sample yields NULL) |
| Linear interpolation onto the grid (interpolate, fill(linear), series_fill_linear, GAP_FILL linear) | Timescale; MetricsQL; Graphite; Datadog; InfluxQL; Kusto; BigQuery; Timestream | K+ | resample mode INTERPOLATE (GAUGE|ANY only; both neighbours within next − prev ≤ lookback, else NULL; a stale marker on either side ends it); DuckDB fill() repairs NULLs in existing rows and extends the nearest value at partition ends, a different edge rule |
| Nearest-neighbour fill (SPLICE, nearest) | QuestDB SPLICE JOIN | K+ | resample mode NEAREST (|t − s| ≤ lookback; equal distance → the carried sample) |
| Cubic-spline interpolation | Timestream interpolate_spline_cubic | P | series_fill_spline in the SERIES family |
| Constant fill / transformNull / fill(zero) / default_zero | Graphite; Datadog; InfluxQL; OpenTSDB zero | D | COALESCE on the grid join |
| Gap-filling that creates rows (time_bucket_gapfill, WITH FILL, SAMPLE BY FILL, GAP_FILL) | Timescale; ClickHouse; QuestDB; BigQuery | C | grid LEFT JOIN, or the operator's output which is already dense on the grid |
| Interpolation bounded by a max gap (fill limit, WITH FILL STALENESS, TOLERANCE) | Datadog fill limit; ClickHouse; QuestDB | K | lookback bound applies to every resample mode |
| xFilesFactor (minimum non-null fraction per bucket) | Graphite | C | count_over_time ÷ expected samples |
| Fill policy participation in aggregation (nan skipped vs zero counted) | OpenTSDB | C | COALESCE vs NULL before GROUP BY; contract documents NULL semantics |

## 10. Temporal alignment joins

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| ASOF JOIN (backward, per key) | kdb+ aj; DuckDB; QuestDB; ClickHouse; Snowflake; pandas merge_asof | D | ASOF JOIN |
| ASOF with tolerance / max staleness | QuestDB TOLERANCE; Snowflake (none); DuckDB (none) | C | ASOF JOIN + WHERE l.t − r.t ≤ tol; on a grid, resample's lookback |
| Forward ASOF, strict LT, SPLICE (nearest either side) | Snowflake MATCH_CONDITION; QuestDB LT/SPLICE JOIN | C | ASOF JOIN … ON l.t <= r.t (any comparison operator is allowed in ON; only USING implies >=); ASOF is inner unless ASOF LEFT JOIN; duplicate right-side timestamps pick an unspecified row, so the consumer deduplicates first; on a grid, resample NEXT/NEAREST (K+) |
| Window join (aggregate right rows in a per-row window; wj reads the prevailing value before the window, wj1 does not) | kdb+ wj/wj1; Timescale LATERAL | K+ | anchored folds: the operator takes an anchor relation (per-series sorted anchors) instead of a regular grid, for edge-reading folds only; kdb+ wj ≡ ANCHOR, wj1 ≡ INSIDE — conditional on whether a two-child sink is expressible for the operator — a registered row if so, a candidate (P) otherwise |
| Interval / temporal join (overlaps) | SQL:2011 period predicates | D | range join on (start, end) |
| Cross-series alignment before binary ops (vector matching) | PromQL on/ignoring/group_left | C | resample both sides to the grid, JOIN on (t, key) |

## 11. Time-weighted and integral

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Time-weighted average (LOCF and linear methods) | Timescale time_weight; QuestDB twap; Kusto (none) | K+ | time_weighted_avg_over_time(method LOCF|LINEAR) (RAW_WINDOW, ANCHOR/SMOOTH eligible, SUM_ABS) |
| Integral (trapezoidal / step) over a window | MetricsQL integrate; Datadog integral; InfluxQL INTEGRAL; Timestream integral_trapezoidal; Timescale time_weight integral | K+ | integral_over_time(method); same fold state as above |
| Interpolated time-weight across bucket edges | Timescale interpolated_average/integral | K+ | the ANCHOR/SMOOTH edge modes on the two rows above |
| rate_over_sum, throughput (sum ÷ window) | MetricsQL; Datadog throughput | C | sum_over_time / range |
| VWAP / weighted averages; OHLC bars | Timescale candlestick_agg; QuestDB weighted_avg; MetricsQL rollup_candlestick | C / K+ | OHLC = the first/last/min/max fields of m4_over_time (K+); vwap = sum_over_time(v·w) / sum_over_time(w) over a second value column (C) — no kernel row carries two values |

## 12. Mergeable state and rollups

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Mergeable partial state stored and finalised later (-State/-Merge, rollup(), sketches as columns) | ClickHouse; Timescale toolkit; Druid; BTrDB stat records | K | _state/_merge/_finalize per row, generated by the registry macro; serialised layout versioned in the consumer contract (T3.6) |
| Continuous aggregates / downsampling tasks / TSDS downsampling | Timescale; InfluxDB; Elastic; Thanos | S | GROUP BY time_bucket over _state into a table; DuckLake/RawDuck keep it (T7.4) |
| Multi-resolution stat tree (count/min/mean/max at every power-of-two) | BTrDB aligned_windows | C | hierarchical GROUP BY over m4/stats _state; power-of-two grids are just grids |
| Several folds of the same window in one pass (aggr_over_time, quantiles_over_time, rollup_candlestick, series_stats) | MetricsQL; Kusto; Timescale accessors | K+ | multi-fold operator surface: ts_fold(window, [rows…]) → STRUCT; one window walk, one buffer; det, scale_kind and domain per member |
| Rollup of a fold's min/max/avg over sub-windows (rollup_rate, rollup_delta, rollup_scrape_interval) | MetricsQL rollup_* | C | subquery: fold (or interval_over_time) on a fine grid, then SLICE folds on the coarse grid |
| Whole-range statistics as a constant series (range_avg, range_max, range_quantile, …) | MetricsQL range_* | D | unbounded window frames over the fold output |
| Versioned / changed-range queries for incremental recomputation | BTrDB | S | DuckLake snapshots |

## 13. Window shapes: tumbling, sliding, session

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Tumbling windows (range = step) | CQL; Dataflow; Scotty; SAMPLE BY; GROUP BY time() | K | grid with range = step (SLICE folds) |
| Hopping / sliding windows (range > step); trailing moving_* over duration | CQL Range/Slide; Graphite movingAverage('1h'); Datadog moving_rollup; Flux timedMovingAverage | K | grid with range > step; RAW_WINDOW or SLICE folds |
| Moving windows by point count (movingAverage(10), median_5, ROWS frames) | Graphite; Datadog; kdb+ mavg | D | window functions with ROWS frames over the sample stream |
| Session windows (gap-defined, merging) | Dataflow; Flink; TSBS long-driving-sessions | D / K+ | lag()+cumulative sum assigns session ids (D); active_duration_over_time(max_gap) (K+, class 20) answers the common questions without materialising sessions |
| Count-based and punctuation windows | CQL Rows N; Scotty | D | ROWS frames; lag() |
| Sliding-window aggregation algorithms (Two-Stacks, DABA, FiBA, Hammer Slide) | Tangwongsan–Hirzel–Schneider | K | internal: SLICE partials are stream slicing (Scotty); RAW_WINDOW walks with two pointers; algebraic class is the state class |
| Out-of-order arrival | FiBA; TSMS survey Insert vs Append | K / S | partition sort in the operator; dedup policy; storage sorts on optimize |
| Event-time vs processing-time, watermarks, triggers, retractions | Dataflow; CQL Istream/Dstream | X | streaming runtime concerns; the kernel is batch over a relation (a consumer may re-run a window) |

## 14. Visualization downsampling

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| M4 (argmin, argmax, first, last per pixel column) | Jugel VLDB 2014; Timescale; Grafana | K+ | m4_over_time → STRUCT(t_min,v_min,t_max,v_max,t_first,v_first,t_last,v_last) (SLICE, mergeable, D0) |
| MinMax (2 points per bucket) with error-bounded caching | Maroulis PVLDB 2024 | K+ | subset of m4_over_time; cache is the _state rollup |
| LTTB / gap-preserving LTTB | Steinarsson 2013; Timescale lttb/gp_lttb; ClickHouse largestTriangleThreeBuckets | P | series_lttb over the series value (sequential, non-associative); MinMaxLTTB = m4 preselection (K+) then series_lttb |
| ASAP automatic smoothing (roughness min s.t. kurtosis preserved) | Rong & Bailis VLDB 2017; Datadog autosmooth; Timescale asap_smooth | P | series_asap (needs ACF, kurtosis, moving mean from classes 6, 15, 16) |
| Parsimonious temporal aggregation (k-interval optimum) | Gordevičius EDBT 2009 | X | research-only; M4/LTTB cover the practical need |

## 15. Smoothing and filters

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Simple moving average / median / min / max / sum over duration | Graphite moving*; Datadog moving_rollup; kdb+ | K | sliding grid with avg/quantile/min/max/sum_over_time |
| EMA, DEMA, TEMA, single exponential smoothing | Graphite exponentialMovingAverage; Datadog ewma_N; Flux; InfluxQL; ClickHouse exponentialMovingAverage; MetricsQL smooth_exponential | P | series_ema/dema/tema/exp_smooth over the regular series value (IIR special cases) |
| Time-decayed sums/averages (exponentialTimeDecayed*) | ClickHouse | P | series_time_decayed_*; or K+ fold with decay parameter if measured cheaper |
| FIR filter (weights, normalize, center) | Kusto series_fir | P | series_fir |
| IIR filter (a, b coefficients) | Kusto series_iir | P | series_iir |
| Rolling standard deviation (stdev(points, tolerance)) | Graphite | K | stddev_over_time on a sliding grid; tolerance = xFilesFactor (C) |
| Technical indicators (RSI, Chande, Kaufman AMA/ER, TRIX) | InfluxQL/Flux | P | series_rsi etc., an optional tier composed from series_ema and lag |

## 16. Spectral and correlation

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| FFT / IFFT | Kusto series_fft/ifft; ClickHouse (internal) | P | series_fft, series_ifft |
| Autocorrelation, lagged cross-correlation (argmax over lag) | ASAP; Kusto (manual); no engine has lagged CCF | P | series_acf, series_ccf(lag range) |
| Pearson correlation of two series (lag 0) | TSM-Bench Q7; Timestream correlate_pearson; Kusto; Flux pearsonr | D | corr() after aligning both to the grid; list_cosine_similarity, list_dot_product on series values |
| Period detection / validation | ClickHouse seriesPeriodDetectFFT; Kusto series_periods_detect/validate | P | series_periods_detect, series_periods_validate |

## 17. Decomposition, trend, forecast

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Linear regression slope, intercept, prediction (deriv, predict_linear, trend_line, linearRegression, series_fit_line) | PromQL; MetricsQL; Datadog; Graphite; Kusto; ClickHouse DerivToGrid/PredictLinearToGrid | K | deriv, predict_linear (SLOPE_COND); regr_* in DuckDB for row-level |
| deriv / predict_linear on native histograms | PromQL | X | the reference rejects histogram inputs (float-only); ChronoDuck does the same |
| Robust regression (Huber), polynomial fit | Datadog robust_trend; Kusto series_fit_poly | P | series_fit_poly, series_fit_robust |
| Holt linear (double exponential smoothing) | PromQL double_exponential_smoothing; MetricsQL holt_winters | K | double_exp_smoothing |
| Holt–Winters seasonal forecast with bands / aberration | Graphite holtWintersForecast/ConfidenceBands/Aberration; InfluxQL HOLT_WINTERS; Elastic moving_fn holtWinters; Datadog forecast seasonal | P | series_holt_winters(α,β,γ,period,horizon) with bands |
| STL decomposition | Cleveland 1990; ClickHouse seriesDecomposeSTL | P | series_decompose_stl (chDB oracle available) |
| Light decomposition (seasonal + line-fit trend + residual + baseline), seasonal component, decomposition forecast | Kusto series_decompose/seasonal/decompose_forecast | P | series_decompose_additive (period by detection or given; seasonal = per-phase mean; trend = least-squares line on the deseasonalised series; residual = x − seasonal − trend; baseline = seasonal + trend, written out in the brief with a worked example), series_seasonal, series_decompose_forecast |
| Two-segment fit (single change point by R²) | Kusto series_fit_2lines | P | series_fit_2lines |
| ttf (time until value reaches zero) | MetricsQL ttf | C | −value / deriv or predict_linear inverse |
| Prophet-style additive models | Taylor & Letham 2018 | X | not a query operator in any engine |

## 18. Anomaly, outlier, change point

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Tukey-fence outlier score | ClickHouse seriesOutliersDetectTukey; Kusto series_outliers; MetricsQL outlier_iqr_over_time | P / C | series_outliers_tukey; the windowed form is quantile_over_time + last_over_time (C) |
| Decomposition-based anomaly scoring (residual + Tukey; S-H-ESD shape) | Kusto series_decompose_anomalies; Datadog anomalies; Twitter S-H-ESD | P | series_decompose_anomalies |
| z-score / MAD outliers over a window or the range | MetricsQL zscore_over_time, range_zscore, range_trim_*; Datadog outliers MAD | C | avg/stddev/mad folds and arithmetic |
| Hoeffding bounds | MetricsQL | C | class 6 |
| Change-point detection (PELT, BOCPD) | Killick 2012; Adams & MacKay 2007; no engine | P | series_changepoints(pelt|bocpd) |
| Group outliers (DBSCAN) | Datadog outliers DBSCAN | X | clustering across series; not a per-series primitive — consumer or ML layer |
| Distribution comparison between buckets (KS test, bucket correlation) | Elastic bucket_count_ks_test/bucket_correlation | P | series_ks_test (optional) |
| Selection-vs-baseline attribute comparison | TraceQL compare() | C | two GROUP BYs over hist_from_values / counts |

## 19. Histograms and sketches

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Classic histogram quantile from le buckets, including rate() outputs (float, non-monotone) | PromQL histogram_quantile(rate(_bucket[5m])); MetricsQL | K / K+ | hist_from_classic accepts DOUBLE counts and applies the reference's monotonicity repair (ensureMonotonicAndIgnoreSmallDeltas) as a Tier 5 primitive; then hist_quantile |
| Native/exponential histogram algebra (add, sub, downscale, compact, reset) | Prometheus native; OTel exponential | K | hist_* primitives and folds |
| Native-histogram rate across a schema change inside the window | Prometheus | K | the window is reduced to its minimum schema before the fold (the pre-scan the comparator section describes); hist_rate |
| Quantile over mixed classic and native series | PromQL histogram_quantile | C | two GROUP BYs (hist_from_classic, native) unioned before hist_quantile |
| Approximate top-k over log streams | Loki approx_topk | K+ | approx_top_k_over_time (class 19) |
| histogram_quantile, quantiles, fraction/share, count, sum, avg, stddev, stdvar | PromQL; MetricsQL histogram_share | K | hist_quantile, hist_fraction, hist_count/sum/avg/stddev/stdvar |
| Histogram built from raw values in a window (histogram_over_time, histogram(), TDIGEST/DD sketch) | MetricsQL; TraceQL histogram_over_time; ClickHouse quantileDD; Timescale uddsketch | K | hist_from_values(scale) for base-2 exponential buckets (TraceQL histogram_over_time = scale 0); hist_from_values(bounds) overload for other layouts (MetricsQL vmrange, base 10^(1/18)); quantile via hist_quantile |
| Approximate percentile with t-digest / HDR / KLL | DuckDB approx_quantile; OpenTSDB ep*; Elastic; Druid; Timescale tdigest | D / K | approx_quantile at a grid point over GROUP BY (t-digest merges under parallel aggregation but has no storable _state and is not order-independent); hist_from_values for storable, order-independent relative-error quantiles |
| Approximate distinct count over a window (HLL), mergeable | ClickHouse uniqHLL12; Timescale hyperloglog; Elastic cardinality; ES|QL COUNT_DISTINCT_OVER_TIME | K+ | approx_distinct_over_time (SKETCH: HLL registers with the pinned xxh3 hash, sparse below a threshold, max-merge so D0; oracle compares the estimate with the exact count within 1.04/√m) |
| Heavy hitters / topN / frequency over a window (space-saving, count-min) | Timescale freq_agg/topn; DuckDB approx_top_k | K+ | approx_top_k_over_time → LIST(STRUCT(key, count_lo, count_hi)), class D2(ε): the exact count lies in [lo, hi] and every key above N/k appears under every partition |
| Bucket conversion and limiting (prometheus_buckets, buckets_limit) | MetricsQL | K | hist_from_classic, hist_downscale, hist_compact |
| Equal-width histogram of values | Timescale histogram(); DuckDB histogram | D | histogram(x, bins) with equi_width_bins (DuckDB ≥ 1.1) over a grid point; plain histogram() is value → count |

## 20. Sessions, states, liveness

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Duration a series was present within a window, ignoring gaps larger than max_gap | MetricsQL duration_over_time; Timescale heartbeat_agg uptime; TSBS daily-activity | K+ | active_duration_over_time(max_gap, tail NONE|EXTEND) (RAW_WINDOW); NONE sums gaps ≤ max_gap between consecutive samples (MetricsQL), EXTEND unions [t_i, t_i + max_gap) (heartbeat liveness) |
| Dead/live ranges, number of gaps | Timescale heartbeat_agg | K+ | gaps_over_time(max_gap) count (same fold); ranges are C via lag() |
| Duration in each discrete state; state at t; state timeline | Timescale state_agg/timeline_agg; Flux stateDuration; TSBS breakdown-frequency | K+ | state_duration_over_time(state) over a CATEGORICAL domain (LOCF between samples, ANCHOR eligible); state_at = resample on the state column (K) |
| Consecutive count in a state; state transitions | Flux stateCount, stateTracking; TSBS breakdown-frequency | K / D | changes on the state column (K, CATEGORICAL); runs via window functions |
| Sessionization and per-session aggregates | TSBS long-driving-sessions, avg-daily-driving-session | D | lag()-gap session ids then GROUP BY |
| Union length of intervals; peak concurrency | ClickHouse intervalLengthSum, maxIntersections | D | sweep over ±1 events with cumulative sum |
| Instantaneous temporal aggregation over interval-stamped data | Böhlen–Gamper–Jensen ITA | D | sweep over endpoints; span aggregation is time_bucket |

## 21. Row-pattern recognition and CEP

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| MATCH_RECOGNIZE row-pattern recognition: PATTERN with quantifiers, PERMUTE, exclusion {- -}, anchors ^ $, SUBSET, DEFINE, MEASURES with CLASSIFIER()/MATCH_NUMBER(), ONE|ALL ROWS PER MATCH (WITH UNMATCHED ROWS), AFTER MATCH SKIP, WITHIN | SQL:2016 §R; Oracle; Snowflake; Flink; Trino | P | an NFA operator over partition-sorted rows with the full standard clause set (an omitted clause is a roster gap, never silent) — separate from the fold operator; DuckDB has nothing |
| Sequence match/count, funnel, retention | ClickHouse sequenceMatch/windowFunnel/retention | P | specialisations of the pattern operator; funnel is also expressible with window functions (slow) |
| Negation / absence within a window | SASE; Cugola & Margara | P / C | the absent-on-grid case is present_over_time + grid join today |

## 22. Similarity and matrix profile

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| DTW distance with Sakoe–Chiba band and LB_Keogh | Keogh 2005; UCR Suite | P | series_dtw |
| PAA / SAX / iSAX symbolic representation and lower-bounding distance | Lin 2003; Shieh & Keogh 2008 | P | series_paa, series_sax (indexing is S) |
| z-normalised Euclidean distance, cosine, dot product | Kusto series_cosine_similarity; Matrix Profile | D / P | list_cosine_similarity, list_distance in DuckDB; series_znorm (P) |
| Matrix profile: motifs, discords (STOMP, streaming STOMPI) | Yeh & Keogh 2016 | P | series_matrix_profile(m, excl = ⌈m/4⌉) (STOMP); the σ = 0 subsequence case is a declared divergence |

## 23. Series-as-value algebra

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Regular series as one value (make-series; timevector; create_time_series) | Kusto; Timescale; Timestream | K | the operator's LIST output surface with the grid descriptor (the output shape — one row per grid point, or one LIST per series — is an open question; see docs/design/architecture.md) |
| Element-wise arithmetic and comparison on series values | Kusto series_add/…; MetricsQL binary ops | D | list_transform / list_zip lambdas |
| Vector ops: dot, cosine, magnitude, sum, product | Kusto | D | list_dot_product, list_cosine_similarity, list_aggregate |
| Unnest series back to rows / rows to series | Kusto mv-expand; Timestream unnest; Timescale unnest | D | unnest(recursive), list() with grid_index |
| Series stats with argmin/argmax indices | Kusto series_stats | K+ | multi-fold STRUCT (class 12) or list_position(list_min) (D) |

## 24. Time and shape transforms

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| offset, @, timeShift, timeStack, calendar_shift with DST | PromQL; Graphite; Datadog | C / D | shift the grid; interval arithmetic with ICU timezone |
| Subqueries (fold over the output of a fold) | PromQL [1h:1m]; MetricsQL | K | the operator accepts any relation; fold output is samples |
| hitcount (rate → counts), scaleToSeconds | Graphite | C | multiply by step |
| summarize / smartSummarize (re-bucket with alignment) | Graphite | K / D | SLICE folds on a coarser grid; time_bucket alignment |
| Calendar decomposition (hour, day_of_week, …), hourSelection | PromQL; Flux | D | date_part |
| Synthetic series (time(), sin, randomWalk, rand_*) | Graphite; MetricsQL | D | generate_series, random() |
| Bitmap ops, ru(), clamp, round, math | MetricsQL; PromQL | D | DuckDB scalars |

## 25. Labels, identity, metadata

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Label rewrite/join/replace/keep/drop; sort_by_label | PromQL; MetricsQL; ClickHouse timeSeriesTags* | D | MAP/STRUCT and string functions; ORDER BY |
| Series identity hash / series id | Prometheus; ClickHouse timeSeriesIdToTags | K | schema profile: series_id role; the kernel takes a GROUP BY |
| Metric type to suffix; name preservation (keep_metric_names) | ClickHouse; MetricsQL | C | consumer |
| Metadata joins (info(), target_info, dimension tables) | PromQL info; TSBS high-load | D | JOIN |
| Exemplars | OTel; TraceQL with(exemplars) | S | stored beside samples; not a fold |

## 26. Missing-value semantics

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| NULL vs absent vs stale trichotomy | Prometheus; Kusto; TSM-Bench | K | stale role/payload; NULL = no value on the grid point (the one documented normalisation); absent = no row |
| Duplicate timestamps policy | QuestDB LT JOIN; Prometheus; ChronoDuck | K | keep-greatest under totalOrder; NaN loses; stale loses to NaN |
| Window boundary convention (left-open, right-closed) and bucket boundary | PromQL; Timescale (varies) | K | Window.contains is the single home of anchor − w < t ≤ anchor; RANGE_CONVENTION divergence for oracles |
| Timestamp precision and units (ms, µs, ns) | OTel ns; Prometheus ms | K | profile: t column role and unit |

## 27. Model-based and approximate queries

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| Queries over models / segments with error bounds (ModelarDB Segment View) | ModelarDB VLDB 2018 | X | storage-format concern; a model-backed table could still feed the operator rows |
| Approximate query processing with confidence intervals (sampling) | BlinkDB-style | X | not a TS primitive; DuckDB TABLESAMPLE |
| Error-bounded results from sketches | uddsketch; hist_quantile bucket bounds | K | hist_quantile's error is the bucket; documented |

## 28. Benchmark query classes

| Primitive | Appears in | Where | How the stack answers it |
|---|---|---|---|
| TSBS single-groupby-*, cpu-max-all, double-groupby-* | TSBS DevOps | D / K | time_bucket GROUP BY (D); or SLICE max/avg on a grid (K) — the structural benchmark shape |
| TSBS lastpoint, groupby-orderby-limit, last-loc, low-fuel, high-load | TSBS | D | arg_max, QUALIFY, JOIN |
| TSBS stationary-trucks, avg-load, avg-vs-projected | TSBS IoT | D / K | avg_over_time on a sliding grid + HAVING |
| TSBS long-driving-sessions, long-daily-sessions, avg-daily-driving-session, daily-activity, breakdown-frequency | TSBS IoT | K+ | active_duration_over_time, state_duration_over_time, changes (class 20) |
| TSM-Bench Q4 downsampling, Q5 upsampling, Q7 correlation | TSM-Bench 2023 | K / K+ / D | SLICE folds; resample INTERPOLATE (K+); corr() |
| IoTDB-Benchmark latest-point, groupby-with-time-filter, value-filter aggregation | IoTDB-Benchmark | D | arg_max; GROUP BY; WHERE |

