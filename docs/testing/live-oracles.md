<!-- scope: the three live SQL-to-SQL oracles that certify grid-aligned folds without a query language -->

# Live oracles without a query language

Three implementations expose grid-aligned folds as plain SQL functions, so parity against them is a SQL-to-SQL differential the kernel can run without knowing what a query language is. They differ in coverage and in independence, and the harness records both.

| Registry family                               | chDB · `timeSeries*ToGrid`                                                                                                                                                     | Timescale Toolkit                                                                                                                 | cerberus `chplan` over chDB                                                                                            |
|-----------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------|
| rate / increase / delta                       | ✓ native, same signature shape (`(start,end,step,window)(ts,v)` → array)                                                                                                       | ✓ `counter_agg` → `extrapolated_rate/delta(method => 'prometheus')` with explicit bounds; per-grid window built by a lateral join | ✓ full, both row path and native path                                                                                  |
| irate / idelta                                | ✓ `InstantRate/DeltaToGrid`                                                                                                                                                    | ✓ `irate_left/right`, `idelta`                                                                                                    | ✓                                                                                                                      |
| resets / changes                              | ✓ `ResetsToGrid`, `ChangesToGrid`                                                                                                                                              | ✓ `num_resets`, `num_changes`                                                                                                     | ✓                                                                                                                      |
| resample (lookback, staleness)                | ✓ `ResampleToGridWithStaleness`                                                                                                                                                | △ `time_bucket_gapfill + locf`: lookback yes, stale marker no                                                                     | ✓                                                                                                                      |
| count/sum/avg/min/max/first/last_over_time    | ✓ native (`timeSeriesCountToGrid`/`SumToGrid`/`AvgToGrid`) for count/sum/avg; △ min/max/first/last remain array-function compositions — no native equivalent in this family      | △ `stats_agg`/native aggregates over lateral windows                                                                              | ✓                                                                                                                      |
| stddev / stdvar / quantile / mad              | △ array functions (`arrayReduce`); exact for stddev, quantile only via `quantileExact`                                                                                         | △ `stats_agg` exact; percentiles approximate only — excluded                                                                      | ✓                                                                                                                      |
| deriv / predict_linear / double_exp_smoothing | ✓ native (`timeSeriesDerivToGrid`/`PredictLinearToGrid`); double_exp_smoothing still unavailable                                                                              | ✓ `slope`, `intercept`; smoothing not available                                                                                   | ✓                                                                                                                      |
| ANCHOR / SMOOTH edge modes                    | ✗                                                                                                                                                                              | △ `interpolated_rate/delta(prev, next)` is SMOOTH-like with explicit neighbours                                                   | ✗ (not lowered)                                                                                                        |
| histogram folds and quantile                  | ✗ (classic quantile via `histogramQuantile`-style array math only)                                                                                                             | ✗                                                                                                                                 | ✓ native and classic, with the exp-histogram scale sweep                                                               |
| Sub-second grids and the `edge` family        | ✓ — `step` and `window` accept fractional and string durations with `DateTime64` input; duplicates: greatest value kept, NaN loses                                            | △ lateral windows are arbitrary; Timescale ranges are `[)`, so both edges differ from `(t − w, t]`                                | ✓                                                                                                                      |
| Independence from the kernel's authors        | High — ClickHouse C++, "mirrors Prometheus algorithms"                                                                                                                         | High — Rust, no shared ancestry with Prometheus or ClickHouse                                                                     | Low — the same author's prior implementation; value is corpus scale and an existing parity record, not a third opinion |
| Where it runs                                 | In-process (`libchdb`), merge gate                                                                                                                                             | Container, release gate                                                                                                           | External derivation tool; emits L2 fixtures with provenance `cerberus-chplan-<sha>`                                    |

✓ certifies the row directly; △ certifies it only through SQL the harness composes (which is then itself a piece of code under the fence: it lives in the oracle target, never imports the kernel, and has its own L1 tables); ✗ cannot certify it, and the roster records the gap rather than the harness skipping it silently.

## Declared divergences

Each oracle carries a closed enum of the ways it is *allowed* to differ from the kernel. The comparator reads the enum; nothing else can excuse a mismatch, and every entry names the behaviour, not a case. Adding an entry is a reviewed change to the enum, with an issue, and the L13 meta-test fails if an enum value is never exercised by any fixture (an unused divergence is a tolerance waiting to be used).

    enum class ChdbDivergence {
      DUP_TS_KEEPS_MAX,        // equal timestamps: ClickHouse keeps the greatest value and a NaN loses to any value, implemented
                               // once (timeseriesMaxValueForDuplicateTimestamp) and shared by every timeSeries* function; the
                               // kernel's totalOrder rule reproduces this, so it is asserted, not excused
      NULL_FOR_TOO_FEW,        // two thresholds, by function family — not a blanket "< 2 samples" rule: RateToGrid/IncreaseToGrid/
                               // DeltaToGrid (combined.count < 2), InstantRate/DeltaToGrid (filled < 2) and DerivToGrid/
                               // PredictLinearToGrid (count < 2 || m2_x == 0) need ≥ 2 samples; ChangesToGrid/ResetsToGrid,
                               // ResampleToGridWithStaleness and SumToGrid/AvgToGrid/CountToGrid need only ≥ 1 (NULL iff the
                               // window is empty). kernel: NULL in both cases — asserted, per function
      NO_STALE_MARKER_INPUT,   // staleness is a parameter, not a NaN payload; fixtures with stale markers are rewritten to gaps for this oracle
    };
    enum class TimescaleDivergence {
      RANGE_CONVENTION,        // Timescale's tstzrange/I64Range bounds are genuinely [) — verified: I64Range::contains in
                               // range.rs is `pt >= l && pt < r`. But the 'prometheus' method's edge arithmetic
                               // (MetricSummary::prometheus_delta, counter-agg/src/lib.rs) treats the two edges
                               // asymmetrically: duration_to_start = first.ts − L, raw, no adjustment (left-closed).
                               // duration_to_end = (H − last.ts) − 1ms, a hardcoded microsecond-precision 1ms subtraction —
                               // the extrapolation's effective right edge is H − 1ms, not H. To get a Prometheus-style
                               // window closed at time T, callers must pass bounds = [L, T + 1ms) (the toolkit's own
                               // doctest does exactly this and says so). Net effect once that convention is followed:
                               // Timescale's effective window for this method is [L, T] — closed on both ends — vs. the
                               // kernel's (t − w, t]. The conventions disagree ONLY at the left edge: a sample landing
                               // exactly at t − w counts for Timescale (duration_to_start = 0, no extrapolation needed
                               // there) but is excluded by the kernel (window is left-open). At the right edge both agree:
                               // a sample exactly at t counts for both (kernel: right-closed; Timescale: the +1ms/−1ms
                               // pair makes duration_to_end = 0 there too). A fixture with a real sample at exactly t − w
                               // exercises this divergence; the right edge needs no excuse since the two conventions
                               // already agree there.
      NO_STALE_MARKER,         // locf has no staleness concept; stale fixtures excluded, recorded as gap
      LATERAL_WINDOW,          // per-grid windows are built by the harness; the window comparison (open/closed) is the harness's and is L1-tested
    };
    enum class ChplanDivergence {
      ROW_PATH_ULP,            // the array-join row path reorders sums; within kReorderTolerance — asserted by the comparator, listed for provenance
      NATIVE_PATH_LAST_BIT,    // the timeSeries* native path differs from the Go reference in the last bit; same
    };

The chDB family returns `Array(Nullable(Float64))` but computes in the input's type (the documentation's own example computes in `Float32`), so an L1 test on the SQL template asserts the harness passes `Float64`; a Float32 leg would be a harness bug, not a divergence.

Note what is *not* in these enums: no per-fixture exclusions, no per-function skips, no "known failing" list. A fixture an oracle cannot evaluate at all (a histogram fixture against Timescale) is not a divergence; it is a roster gap, recorded as ✗ above and counted by the L12 provenance floor.

## The harness shape

One driver, three back-ends, one fixture format. For each fixture the driver loads `samples` into a table in every back-end, asks each for the grid through a per-back-end SQL template (the kernel's own function; chDB's `timeSeries*` or composed array SQL; Timescale's toolkit over a lateral window), and compares each oracle's array to the kernel's under the comparator, applying only that oracle's enum. Results are rostered by `(fixture id, oracle)` with the four fatal verdicts. The chDB leg runs in-process on every PR; the Timescale leg runs on release with testcontainers; the `chplan` leg is not a live oracle in this lane at all — it is a derivation tool that feeds L2, so that its output is checked into the repository as fixtures and its Go dependency never enters the kernel's build. The whole `timeSeries*ToGrid` family is gated behind ClickHouse's `allow_experimental_time_series_aggregate_functions` setting; the chDB harness sets it per session before issuing any of the SQL templates above.
