<!-- scope: the complete function surface — the registry macro table, what the kernel deliberately does not own, and the SERIES/PATTERN family extension that closes the surface against a full TSDB query layer -->

# Function surface

The registry is the complete function surface, not a sample. It was derived by walking the range, instant and histogram functions of the three consumers and keeping every distinct *fold*; where two consumers spell the same fold differently (a log engine's `rate` is a metric engine's `count_over_time / range`) only the fold is registered and the arithmetic is the consumer's. Names are the kernel's own; nothing here names a consumer.

    // kernel/registry.def — the single source of truth (v1 surface)
    //
    // family      RANGE   fold over samples in (t − window, t] at each grid point
    //             INSTANT value carried to each grid point from the last sample within lookback
    //             HIST    histogram-valued input or output
    //             GRID    helpers that build or inspect the grid itself
    //             SERIES  scalar over a regular series value: LIST(DOUBLE) plus the grid descriptor (start, step, n)
    //             PATTERN row-pattern recognition over partition-sorted rows (its own operator)
    // state       RAW_WINDOW  samples folded in timestamp order; operator form holds one window,     → D0 by construction
    //                         aggregate form holds the scan bound (documented O(range))
    //             SLICE       one order-free partial per grid bucket                                 → D0 for count/min/max, D1 for sums
    //             LAST_K(n)   the n newest samples by timestamp                                      → D0
    //             HIST_WINDOW RAW_WINDOW of histogram structs                                        → D0
    //             HIST_MERGE  one running histogram, bucket-wise add after downscale                → D0 for integer counts, D1 for float
    //             SKETCH      order-free mergeable summary per grid bucket (HLL registers, space-saving) → D0 for HLL (max-merge, pinned xxh3), D2(ε) for space-saving
    //             SERIES      the whole regular series resident; sequential evaluation in pinned order   → D0 within a build; across platforms with -ffp-contract=off and a shipped twiddle table
    //             NFA         partial matches resident, bounded by a mandatory WITHIN                    → D0
    // det         D0 bit-identical under any partition · D1 within the reorder bound · D2(ε) within a documented sketch bound, output shaped so the bound is checkable
    //             static rule: any state whose partial contains a float sum is D1 unless it uses reproducible_sum — applies to SLICE, HIST_MERGE and SKETCH alike
    // layers      the family → layers map (one line per family; L13 iterates this map, never a flat "every layer"):
    //             RANGE, INSTANT, HIST, GRID → L0–L14 as the layer map states
    //             SERIES → L0, L1a–c, L2, L3 (the series/ oracle module), L5, L6a where an oracle exists, L9, L10, L12, L13, L14,
    //                      L4′ (bit-identity across require vector_size builds, thread counts and preserve_insertion_order; no combine), L11′ (temporary-allocation law)
    //             PATTERN → as SERIES plus the NFA fuzz lane; L4′ over partitions (MR-PATTERN-PARTITION)
    // edge        INSIDE      only samples inside the window
    //             EXTRAPOLATE project to both edges from the inside samples (threshold 1.1×, zero clamp)
    //             ANCHOR      also read the last sample before the window
    //             SMOOTH      interpolate the value at both edges
    //             LOOKBACK    carry the newest sample not older than lookback; a stale marker ends the carry
    //             INTERPOLATE linear between prev (newest at or before t) and next (first after t); requires next − prev ≤ lookback, else NULL; GAUGE|ANY only, rejected on CATEGORICAL at bind
    //             NEXT        carry the first sample after t with next − t ≤ lookback, else NULL (INSTANT only)
    //             NEAREST     the closer of prev and next within lookback; equal distance → prev (degrades to LOOKBACK) (INSTANT only)
    //             for all four: a stale marker on either side ends the mode — the series was absent between, never read through (MR-STALE, MR-STALE-NEXT)
    // domain      COUNTER monotone with resets · GAUGE arbitrary · NONNEG ≥ 0 · ANY · HISTOGRAM struct · CATEGORICAL discrete state (VARCHAR/INTEGER)
    //             CATEGORICAL rules: ties and "smallest" are bytewise (memcmp) regardless of the session collation; MAP outputs emit keys in that order;
    //             NULL state rows are dropped before the fold (documented; L5 has the statement pair); INTERPOLATE is rejected at bind
    //             COUNTER reset_policy (one enum on the reset-predicate plug; the pipeline is wrap → (st_reset ∨ value drop) → policy):
    //               RESTART  default — the reset contributes cur (the counter restarted from zero)
    //               ZERO     the reset contributes 0 (ClickHouse deltaSum; a positive-delta cumsum)
    //               NULL     the grid point is NULL (Graphite nonNegativeDerivative without maxValue)
    //               DROP     the sample is dropped before the fold (OpenTSDB dropResets); an st_reset without a value drop is not dropped
    //               WRAP(counter_max, counter_min = 0, inclusive = false)  delta = counter_max − prev + cur − counter_min (+1 when inclusive: Graphite; OpenTSDB is exclusive)
    //             orthogonal options: reset_value (a per-second rate above it is emitted as 0; increase compares rate × window, then scales) ·
    //             zero_start (the no-start-timestamp spelling of the single-sample rule; when a start timestamp is present, st wins — MR-ST row)
    // forms       grid      the operator over (start, end, step); RANGE rows also accept range = step (tumbling) and range > step (sliding)
    //             bounded   the aggregate form with explicit (window_start, window_end) per group — edge-reading folds only (RAW_WINDOW, LAST_K,
    //                       HIST_WINDOW; static check); INSIDE SLICE folds over calendar buckets are DuckDB's GROUP BY time_bucket. Scan bound pushed
    //                       only when the bounds are constants (a VALUES list or a scalar subquery): [min(start) − window − lookback, max(end)];
    //                       otherwise the form is pushdown-ineligible and the two-gate law is asserted absent (as for table-function sources)
    //             anchored  the operator over an anchor relation (per-series sorted anchors) instead of a regular grid — the window join (kdb+ wj ≡
    //                       ANCHOR, wj1 ≡ INSIDE); edge-reading folds only; law: resident O(threads × window), walk O(n + anchors), the anchor child a
    //                       second spillable sort. Whether a two-child sink is expressible for the operator is an open question; until it is answered
    //                       this form is a candidate, not a row
    //             multi     fold(window, [rows…]) → STRUCT, one window walk for several rows; det, scale_kind and domain per member; a member's edge
    //                       mode is validated against the requested one at bind. fold(sum) ≡ sum_over_time within the D1 bound until sums retire D1

    // scale_kind (comparator conditioning, see docs/testing/comparator.md): EXACT | SUM_ABS | SUM_ABS_TIMES_FACTOR | RESIDUAL_SS | SLOPE_COND | LIBM
    //             | LIBM_EXP (scale = |mean(log v)| · result) | NORM2_LOGN (scale = ‖x‖₂ · log₂ n, FFT-derived rows) | RECURRENCE(n) (scale grows with
    //             the recurrence length; re-anchored against brute force every k steps) · sqrt is correctly rounded and therefore EXACT, never LIBM
    //             LIBM rows are listed, not inferred: geomean_over_time, series_time_decayed, series_ks_test (p-value), series_ema by half-life (pow)
    // static-checked against state/det: every selection is EXACT; every extrapolated counter is SUM_ABS_TIMES_FACTOR
    //        name                      family   state        det  edge modes                   domain
    // ── counters and deltas ───────────────────────────────────────────────────────────────────────
    // COUNTER-domain rows take an optional start_ts input; the reset predicate is (value drop ∨ st_reset)
    TS_FN(rate,                      RANGE,   RAW_WINDOW,  D0,  EXTRAPOLATE|ANCHOR|SMOOTH,   COUNTER)
    TS_FN(increase,                  RANGE,   RAW_WINDOW,  D0,  EXTRAPOLATE|ANCHOR|SMOOTH,   COUNTER)
    TS_FN(delta,                     RANGE,   RAW_WINDOW,  D0,  EXTRAPOLATE|ANCHOR|SMOOTH,   GAUGE)
    TS_FN(irate,                     RANGE,   LAST_K(2),   D0,  INSIDE,                      COUNTER)
    TS_FN(idelta,                    RANGE,   LAST_K(2),   D0,  INSIDE,                      GAUGE)
    TS_FN(resets,                    RANGE,   RAW_WINDOW,  D0,  INSIDE|ANCHOR,               COUNTER)
    TS_FN(changes,                   RANGE,   RAW_WINDOW,  D0,  INSIDE|ANCHOR,               ANY|CATEGORICAL)
    // ── window reductions ─────────────────────────────────────────────────────────────────────────
    TS_FN(count_over_time,           RANGE,   SLICE,       D0,  INSIDE,                      ANY)
    TS_FN(sum_over_time,             RANGE,   SLICE,       D1,  INSIDE,                      ANY)      // Neumaier; D0 target
    TS_FN(avg_over_time,             RANGE,   SLICE,       D1,  INSIDE,                      ANY)      // compensated mean; D0 target
    TS_FN(min_over_time,             RANGE,   SLICE,       D0,  INSIDE,                      ANY)
    TS_FN(max_over_time,             RANGE,   SLICE,       D0,  INSIDE,                      ANY)
    TS_FN(first_over_time,           RANGE,   SLICE,       D0,  INSIDE,                      ANY)      // by timestamp
    TS_FN(last_over_time,            RANGE,   SLICE,       D0,  INSIDE,                      ANY)      // by timestamp
    TS_FN(stddev_over_time,          RANGE,   RAW_WINDOW,  D0,  INSIDE,                      ANY)      // two-pass over sorted buffer
    TS_FN(stdvar_over_time,          RANGE,   RAW_WINDOW,  D0,  INSIDE,                      ANY)
    TS_FN(quantile_over_time,        RANGE,   RAW_WINDOW,  D0,  INSIDE,                      ANY)      // (φ, method R7|R3) Hyndman–Fan; R7 linear is the default and the convention the oracles share
    TS_FN(mad_over_time,             RANGE,   RAW_WINDOW,  D0,  INSIDE,                      ANY)      // median absolute deviation
    TS_FN(present_over_time,         RANGE,   SLICE,       D0,  INSIDE,                      ANY)      // 1 if any sample; absent is the consumer's grid join
    TS_FN(ts_of_min_over_time,       RANGE,   SLICE,       D0,  INSIDE,                      ANY)      // timestamp of the extremum
    TS_FN(ts_of_max_over_time,       RANGE,   SLICE,       D0,  INSIDE,                      ANY)
    TS_FN(ts_of_first_over_time,     RANGE,   SLICE,       D0,  INSIDE,                      ANY)
    TS_FN(ts_of_last_over_time,      RANGE,   SLICE,       D0,  INSIDE,                      ANY)
    // ── trend and smoothing ───────────────────────────────────────────────────────────────────────
    TS_FN(deriv,                     RANGE,   RAW_WINDOW,  D0,  INSIDE,                      GAUGE)    // least-squares slope
    TS_FN(predict_linear,            RANGE,   RAW_WINDOW,  D0,  INSIDE,                      GAUGE)    // slope + intercept at t + horizon
    TS_FN(double_exp_smoothing,      RANGE,   RAW_WINDOW,  D0,  INSIDE,                      GAUGE)    // Holt–Winters (sf, tf)
    // ── instant carry ─────────────────────────────────────────────────────────────────────────────
    TS_FN(resample,                  INSTANT, LAST_K(1),   D0,  LOOKBACK|INTERPOLATE|NEXT|NEAREST, ANY|CATEGORICAL) // value at each grid point; INTERPOLATE reads edge_context
    TS_FN(resample_ts,               INSTANT, LAST_K(1),   D0,  LOOKBACK|NEXT|NEAREST,       ANY|CATEGORICAL) // timestamp of the carried sample
    // ── histogram-valued range folds ──────────────────────────────────────────────────────────────
    TS_FN(hist_rate,                 HIST,    HIST_WINDOW, D0,  EXTRAPOLATE|ANCHOR|SMOOTH,   HISTOGRAM)
    TS_FN(hist_increase,             HIST,    HIST_WINDOW, D0,  EXTRAPOLATE|ANCHOR|SMOOTH,   HISTOGRAM)
    TS_FN(hist_delta,                HIST,    HIST_WINDOW, D0,  EXTRAPOLATE|ANCHOR|SMOOTH,   HISTOGRAM)
    TS_FN(hist_resets,                HIST,    HIST_WINDOW, D0,  INSIDE|ANCHOR,               HISTOGRAM)
    TS_FN(hist_sum_over_time,        HIST,    HIST_MERGE,  D1,  INSIDE,                      HISTOGRAM)
    TS_FN(hist_avg_over_time,        HIST,    HIST_MERGE,  D1,  INSIDE,                      HISTOGRAM)
    TS_FN(hist_last_over_time,       HIST,    LAST_K(1),   D0,  INSIDE,                      HISTOGRAM)
    TS_FN(hist_first_over_time,      HIST,    SLICE,       D0,  INSIDE,                      HISTOGRAM)
    TS_FN(hist_count_over_time,      HIST,    SLICE,       D0,  INSIDE,                      HISTOGRAM)
    TS_FN(hist_present_over_time,    HIST,    SLICE,       D0,  INSIDE,                      HISTOGRAM)
    TS_FN(hist_changes,              HIST,    HIST_WINDOW, D0,  INSIDE|ANCHOR,               HISTOGRAM) // verified: the reference's changes() walks floats and histograms merged by timestamp
    TS_FN(hist_resample,             HIST,    LAST_K(1),   D0,  LOOKBACK,                    HISTOGRAM)
    // ── histogram construction and cross-series merge ─────────────────────────────────────────────
    TS_FN(hist_merge,                HIST,    HIST_MERGE,  D1,  —,                           HISTOGRAM) // Σ across series at one grid point
    TS_FN(hist_from_values,          HIST,    HIST_MERGE,  D1,  INSIDE,                      NONNEG)    // exponential histogram of raw values in the window: (scale) base-2, or (bounds) for other layouts; its float sum is D1 (D0 target)
    TS_FN(hist_from_classic,         HIST,    HIST_MERGE,  D0,  —,                           NONNEG)    // rows of (upper_bound, cumulative count as DOUBLE) → classic struct after the reference's monotonicity repair (Tier 5 hist_monotone_repair)
    // ── histogram point functions (scalar over one struct) ────────────────────────────────────────
    TS_FN(hist_quantile,             HIST,    —,           D0,  —,                           HISTOGRAM) // φ; exponential interpolation in native buckets, linear in classic
    TS_FN(hist_fraction,             HIST,    —,           D0,  —,                           HISTOGRAM) // share of observations in [lo, hi]
    TS_FN(hist_count,                HIST,    —,           D0,  —,                           HISTOGRAM)
    TS_FN(hist_sum,                  HIST,    —,           D0,  —,                           HISTOGRAM)
    TS_FN(hist_avg,                  HIST,    —,           D0,  —,                           HISTOGRAM)
    TS_FN(hist_stddev,               HIST,    —,           D0,  —,                           HISTOGRAM) // geometric bucket midpoints
    TS_FN(hist_stdvar,               HIST,    —,           D0,  —,                           HISTOGRAM)
    TS_FN(hist_downscale,            HIST,    —,           D0,  —,                           HISTOGRAM) // to a coarser scale, lossless by subsetting
    TS_FN(hist_compact,              HIST,    —,           D0,  —,                           HISTOGRAM) // span merging
    TS_FN(hist_detect_reset,         HIST,    —,           D0,  —,                           HISTOGRAM) // (prev, cur) → bool, the pairwise rule hist_rate folds
    // ── grid helpers ──────────────────────────────────────────────────────────────────────────────
    TS_FN(grid,                      GRID,    —,           D0,  —,                           —)         // table fn: (start, end, step) → rows
    TS_FN(grid_index,                GRID,    —,           D0,  —,                           —)         // scalar: (t, start, step) → bucket
    TS_FN(stale_marker,              GRID,    —,           D0,  —,                           —)         // the NaN payload that ends a carry; is_stale(v)
    TS_FN(last_two_samples,          RANGE,   LAST_K(2),   D0,  INSIDE,                      ANY)       // materialised-view helper for irate/idelta rollups
    // ── surface extension (coverage matrix — docs/design/coverage.md): folds a full TSDB query layer needs; comments cite the algorithm, never a consumer ──
    TS_FN(time_weighted_avg_over_time, RANGE, RAW_WINDOW,  D0,  INSIDE|ANCHOR|SMOOTH,        GAUGE)     // method LOCF|LINEAR (step vs trapezoid integration); SUM_ABS
    TS_FN(integral_over_time,        RANGE,   RAW_WINDOW,  D0,  INSIDE|ANCHOR|SMOOTH,        GAUGE)     // method LOCF|LINEAR; area under the curve; SUM_ABS
    TS_FN(range_over_time,           RANGE,   SLICE,       D0,  INSIDE,                      ANY)       // max − min (MinMax partial)
    TS_FN(sum2_over_time,            RANGE,   SLICE,       D1,  INSIDE,                      ANY)       // Σ v²; D0 target with reproducible_sum
    TS_FN(geomean_over_time,         RANGE,   SLICE,       D1,  INSIDE,                      NONNEG)    // exp(reproducible_sum(log v)/n) once sums retire D1; LIBM_EXP
    TS_FN(mode_over_time,            RANGE,   RAW_WINDOW,  D0,  INSIDE,                      ANY|CATEGORICAL) // ties → smallest under totalOrder
    TS_FN(distinct_over_time,        RANGE,   RAW_WINDOW,  D0,  INSIDE,                      ANY|CATEGORICAL) // exact distinct count
    TS_FN(moments_over_time,         RANGE,   RAW_WINDOW,  D0,  INSIDE,                      ANY)       // STRUCT(skewness, kurtosis); RESIDUAL_SS
    TS_FN(monotone_over_time,        RANGE,   RAW_WINDOW,  D0,  INSIDE|ANCHOR,               ANY)       // kind increases|decreases|ascent|descent
    TS_FN(ts_of_last_change_over_time, RANGE, RAW_WINDOW,  D0,  INSIDE|ANCHOR,               ANY|CATEGORICAL)
    TS_FN(interval_over_time,        RANGE,   RAW_WINDOW,  D0,  INSIDE,                      ANY)       // median inter-sample gap (scrape interval)
    TS_FN(stale_samples_over_time,   RANGE,   SLICE,       D0,  INSIDE,                      ANY)       // count of stale markers
    TS_FN(m4_over_time,              RANGE,   SLICE,       D0,  INSIDE,                      ANY)       // STRUCT(t_min,v_min,t_max,v_max,t_first,v_first,t_last,v_last) — Jugel 2014
    // ohlc is m4_over_time's fields under other names (open = first, close = last); vwap needs a second value column and is the consumer's
    // sum_over_time(v·w) / sum_over_time(w) — no kernel row carries two values (see Deliberately not)
    TS_FN(active_duration_over_time, RANGE,   RAW_WINDOW,  D0,  INSIDE|ANCHOR,               ANY)       // (max_gap, tail): NONE sums consecutive gaps ≤ max_gap (no tail); EXTEND unions [t_i, t_i + max_gap) ∩ window (heartbeat)
    TS_FN(gaps_over_time,            RANGE,   RAW_WINDOW,  D0,  INSIDE|ANCHOR,               ANY)       // number of gaps longer than max_gap
    TS_FN(state_duration_over_time,  RANGE,   RAW_WINDOW,  D0,  INSIDE|ANCHOR,               CATEGORICAL) // MAP(state → duration), LOCF between samples
    TS_FN(approx_distinct_over_time, RANGE,   SKETCH,      D0,  INSIDE,                      ANY|CATEGORICAL) // HLL, pinned xxh3, p default 12, sparse below a threshold; max-merge; oracle = exact count within 1.04/√m
    TS_FN(approx_top_k_over_time,    RANGE,   SKETCH,      D2,  INSIDE,                      ANY|CATEGORICAL) // space-saving → LIST(STRUCT(key, count_lo, count_hi)); exact ∈ [lo, hi]; every key above N/k present under every partition
    TS_FN(fold,                      RANGE,   RAW_WINDOW,  per-member, INSIDE|EXTRAPOLATE|ANCHOR|SMOOTH, per-member) // multi form: several rows from one walk → STRUCT; det, scale_kind, domain per member; a FILTER member serves share_* folds
    // ── SERIES family: scalar over a regular series value; DuckDB list functions own element-wise algebra, dot and cosine ─
    TS_FN(series_fill_forward,       SERIES,  SERIES,      D0,  —,                           ANY)       // LOCF over NULL cells, optional limit
    TS_FN(series_fill_backward,      SERIES,  SERIES,      D0,  —,                           ANY)       // NOCB
    TS_FN(series_fill_linear,        SERIES,  SERIES,      D0,  —,                           ANY)
    TS_FN(series_fill_const,         SERIES,  SERIES,      D0,  —,                           ANY)
    TS_FN(series_fill_spline,        SERIES,  SERIES,      D0,  —,                           ANY)       // natural cubic spline (second derivative 0 at the ends); RESIDUAL_SS
    TS_FN(series_ema,                SERIES,  SERIES,      D0,  —,                           ANY)       // α or span; DEMA/TEMA by parameter; ≡ series_iir(a=[1,−α], b=[α]); RECURRENCE(n)
    TS_FN(series_exp_smooth,         SERIES,  SERIES,      D0,  —,                           ANY)       // single exponential smoothing
    TS_FN(series_time_decayed,       SERIES,  SERIES,      D0,  —,                           ANY)       // kind sum|avg|max|count with half-life; LIBM
    TS_FN(series_fir,                SERIES,  SERIES,      D0,  —,                           ANY)       // weights, normalize, center
    TS_FN(series_iir,                SERIES,  SERIES,      D0,  —,                           ANY)       // a, b coefficients; pinned recurrence order; RECURRENCE(n)
    TS_FN(series_fft,                SERIES,  SERIES,      D0,  —,                           ANY)       // radix-2 with Bluestein fallback (pad ≥ 2n − 1); shipped twiddle table; NORM2_LOGN
    TS_FN(series_ifft,               SERIES,  SERIES,      D0,  —,                           ANY)       // NORM2_LOGN
    TS_FN(series_acf,                SERIES,  SERIES,      D0,  —,                           ANY)       // autocorrelation by lag (denominator n, pinned); NORM2_LOGN
    TS_FN(series_ccf,                SERIES,  SERIES,      D0,  —,                           ANY)       // lagged cross-correlation with argmax lag; NORM2_LOGN
    TS_FN(series_periods_detect,     SERIES,  SERIES,      D0,  —,                           ANY)       // (periods, scores) from FFT + ACF peaks
    TS_FN(series_periods_validate,   SERIES,  SERIES,      D0,  —,                           ANY)
    TS_FN(series_decompose_stl,      SERIES,  SERIES,      D0,  —,                           ANY)       // Cleveland 1990: (seasonal, trend, residual); RESIDUAL_SS
    TS_FN(series_decompose_additive, SERIES,  SERIES,      D0,  —,                           ANY)       // additive-linear: seasonal by per-phase mean, trend by least-squares line — written out in Surface extension
    TS_FN(series_seasonal,           SERIES,  SERIES,      D0,  —,                           ANY)
    TS_FN(series_decompose_forecast, SERIES,  SERIES,      D0,  —,                           ANY)       // baseline extrapolated `points` ahead
    TS_FN(series_holt_winters,       SERIES,  SERIES,      D0,  —,                           ANY)       // (α, β, γ, period, horizon) → forecast, bands, aberration
    TS_FN(series_fit_poly,           SERIES,  SERIES,      D0,  —,                           ANY)       // degree d ≤ 8 on an orthogonal (Chebyshev) basis; RESIDUAL_SS with scale ‖r‖₂ · κ(V)
    TS_FN(series_fit_robust,         SERIES,  SERIES,      D0,  —,                           ANY)       // Huber IRLS line; RESIDUAL_SS
    TS_FN(series_fit_2lines,         SERIES,  SERIES,      D0,  —,                           ANY)       // single change point by R²
    TS_FN(series_outliers_tukey,     SERIES,  SERIES,      D0,  —,                           ANY)       // (q_lo, q_hi, k) → score per cell
    TS_FN(series_decompose_anomalies, SERIES, SERIES,      D0,  —,                           ANY)       // decompose then Tukey on the residual → (flag, score, baseline)
    TS_FN(series_znorm,              SERIES,  SERIES,      D0,  —,                           ANY)       // EXACT (sqrt is correctly rounded); σ = 0 → all zeros
    TS_FN(series_lttb,               SERIES,  SERIES,      D0,  —,                           ANY)       // Steinarsson 2013, sequential; MinMaxLTTB = m4_over_time preselection then this
    TS_FN(series_asap,               SERIES,  SERIES,      D0,  —,                           ANY)       // Rong & Bailis 2017: roughness min s.t. kurtosis preserved; ACF-peak search
    TS_FN(series_dtw,                SERIES,  SERIES,      D0,  —,                           ANY)       // (series_b, band r) Sakoe–Chiba |i − j| ≤ r, mandatory above n = 4 096; LB_Keogh early abandon; EXACT
    TS_FN(series_paa,                SERIES,  SERIES,      D0,  —,                           ANY)
    TS_FN(series_sax,                SERIES,  SERIES,      D0,  —,                           ANY)       // alphabet a, word length w; MINDIST helper
    TS_FN(series_matrix_profile,     SERIES,  SERIES,      D0,  —,                           ANY)       // STOMP streaming, O(n) memory; (m, excl = ⌈m/4⌉); σ = 0 subsequences a declared divergence; RECURRENCE(n)
    TS_FN(series_changepoints,       SERIES,  SERIES,      D0,  —,                           ANY)       // method PELT|BOCPD with penalty / hazard
    TS_FN(series_ks_test,            SERIES,  SERIES,      D0,  —,                           ANY)       // two-sample KS statistic and p-value; LIBM
    // ── PATTERN family: one operator, SQL:2016 clause set ─────────────────────────────────────────
    TS_FN(match_pattern,             PATTERN, NFA,         D0,  —,                           —)         // SQL:2016 §R: PARTITION BY, ORDER BY t, PATTERN (quantifiers greedy/reluctant, alternation, PERMUTE, exclusion {- -}, ^ $), SUBSET, DEFINE, MEASURES (CLASSIFIER, MATCH_NUMBER, FIRST/LAST/PREV/NEXT), ONE|ALL ROWS PER MATCH [WITH UNMATCHED ROWS], AFTER MATCH SKIP (PAST LAST ROW | TO NEXT ROW | TO FIRST/LAST var), WITHIN mandatory
    TS_FN(sequence_match,            PATTERN, NFA,         D0,  —,                           —)         // boolean/count specialisation (ClickHouse sequenceMatch/sequenceCount)
    TS_FN(window_funnel,             PATTERN, NFA,         D0,  —,                           —)         // steps reached within a window
    TS_FN(retention,                 PATTERN, NFA,         D0,  —,                           —)         // cohort condition array

    // Every RANGE / INSTANT / HIST aggregate above also exists as _state, _merge
    // and _finalize — generated by the macro, never written by hand — so partial states
    // are storable, and the L4 partition tests can drive combine explicitly.

## Deliberately not in the kernel

A closure test is only meaningful if the boundary is written down. These are the things the consumers need that the kernel refuses to own, each with the reason.

| Not registered                                    | Why                                                                                                                                                                                                                                                            |
|---------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Cross-series aggregation                          | `sum`, `min`, `max`, `avg`, `count`, `stddev`, `quantile`, `topk`, `count_values` across series at one grid point are plain `GROUP BY` over DuckDB's native aggregates. The one exception is histograms, hence `hist_merge`.                                   |
| Per-second rates of counts and sums               | A log engine's `rate` is `count_over_time / window`; its `bytes_rate` is `sum_over_time(bytes) / window`; a trace engine's `rate` is the same count fold over spans. Registering them would be registering arithmetic. The consumer divides.                   |
| Absence                                           | `absent` and `absent_over_time` produce a value where there are *no rows*; that is a left join against `grid` on the consumer's side, with `present_over_time` as the kernel-side predicate.                                                                   |
| Offset, `@`, subqueries                           | All three are transformations of the grid or of the sample set before the fold — shift the grid, pin it, or feed a previous grid's output back in as samples. The folds are unchanged.                                                                         |
| Scalar math, clamping, rounding, date parts, trig | DuckDB natives (`abs`, `ceil`, `clamp` via `greatest/least`, `ln`, `exp`, `date_part`, …). A consumer that needs a spelling DuckDB lacks (`sgn`, `deg`) writes a macro, not a kernel entry.                                                                    |
| Label operations, sorting, series identity        | `label_replace`, `label_join`, `sort_by_label`, vector matching (`on`/`ignoring`/`group_left`) and the label-set hash are relational: string functions, `ORDER BY` and joins on the consumer's grouping key. The kernel takes a `GROUP BY`, not a label model. |
| Two-value folds (vwap, weighted averages)         | A fold reads one value column. Volume-weighted and weighted averages are `sum_over_time(v·w) / sum_over_time(w)` on the consumer's side; OHLC is `m4_over_time` under other field names.                                                                                                                              |
| Classic-histogram bucket assembly                 | Turning `_bucket` rows with an `le` label into a struct is `hist_from_classic`; recognising which rows *are* buckets is the schema profile's job.                                                                                                              |
| Query-language-specific rollups                   | Variants that one engine adds on top (multi-value rollups, step-relative windows like `[5i]`) are consumer arithmetic over the folds above or grid parameters; a new *fold* would be a new registry row, with the full test discipline attached.               |

The four histogram overloads of `first/count/present_over_time` and `changes` exist because the reference accepts histogram samples in `changes` (verified in source) and `first_over_time` exists there as experimental; `count`/`present` on histograms remain unconfirmed, and a `DOUBLE`-typed `ANY` cannot bind a struct in any case. Two rows in the surface are provisional and marked so in the registry file: `mad_over_time` and the `ts_of_*` family are experimental in their reference and may change; they are registered at full fence strength anyway, because an experimental function with weaker tests is how a tolerance sneaks in.


## Surface extension — the full TSDB query layer

The registry is checked against a different question from row-by-row correctness: is the set of rows the query layer of a full time-series database? The answer is a coverage matrix (`docs/design/coverage.md`): every per-series primitive in PromQL, MetricsQL, LogQL, TraceQL, Graphite, Datadog, OpenTSDB, M3, TimescaleDB core and Toolkit, QuestDB, ClickHouse, InfluxQL/Flux, Druid, Elasticsearch/ES|QL, BigQuery, Snowflake, Timestream, kdb+ and Kusto, and in the CQL, Dataflow, Scotty, SWAG, M4/LTTB/ASAP, BTrDB, ModelarDB, SASE/MATCH_RECOGNIZE, DTW/SAX/Matrix-Profile, STL/S-H-ESD/PELT and TSBS/TSM-Bench literature, is mapped to one of: an existing registry row, a proposed row, a DuckDB native, a consumer composition, the storage layer, or out of the query layer with the reason. 182 primitives; the monitoring surface (PromQL, LogQL, TraceQL, ES|QL, ClickHouse `timeSeries*ToGrid`) is already complete, and MetricsQL's non-extrapolated counters are the `ANCHOR` edge mode. The rest became the rows above the `SERIES` and `PATTERN` families, plus four things that are not rows:

- **Resample modes.** `INTERPOLATE`, `NEXT` and `NEAREST` join `LOOKBACK`; the lookback bound applies to all four, which is what every "fill limit / TOLERANCE / STALENESS" clause elsewhere expresses.
- **Counter reset policy.** The reset predicate is a plug; `counter_max`, `reset_value`, `drop_resets` and `zero_start` are its parameters (Graphite `maxValue`, OpenTSDB `counterMax`/`resetValue`/`dropResets`, MetricsQL `increase_pure`). Each is a fixture family carrying the reference's own numbers.
- **Where a fold is anchored.** Three forms beside the grid, for edge-reading folds only (RAW_WINDOW, LAST_K, HIST_WINDOW; an INSIDE SLICE fold over calendar buckets is DuckDB's own `GROUP BY time_bucket`): the *bounded* aggregate form with explicit `(window_start, window_end)` per group (calendar buckets; the scan bound pushed only for constant bounds), the *anchored* operator form over an anchor relation (the kdb+ window join, `wj` ≡ ANCHOR and `wj1` ≡ INSIDE; "the ten minutes before each event"), and the *multi* form `fold` returning a STRUCT from one walk with det, scale kind and domain per member. All three forms are evaluated together; the anchored form's registration additionally depends on whether a two-child sink is expressible for the operator — until that is resolved it remains a candidate, not a row.
- **A `CATEGORICAL` domain.** Discrete states (VARCHAR/INTEGER) for `state_duration_over_time`, and for `changes`, `resample`, `mode`, `distinct` and the sketches; LOCF between samples; ordered under DuckDB's collation for ties.

The `SERIES` family is Kusto's model — a regular series as a single value — made a scalar family over `LIST(DOUBLE)` with the grid descriptor `(start, step, n)` the operator already emits; element-wise arithmetic, dot product and cosine similarity are DuckDB list functions and are not registered. Its state class `SERIES` is sequential by construction (pinned evaluation order, hence `D0`); transcendental steps carry `LIBM`, least-squares steps `RESIDUAL_SS` or `SLOPE_COND`. The `PATTERN` family is one operator — an NFA with a match buffer over partition-sorted rows (SASE; SQL:2016) — that shares the partition sort and the profile with the fold operator and nothing else.
