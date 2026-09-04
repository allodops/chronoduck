<!-- scope: the logical schema (the profile) the kernel binds against, the canonical physical layout, and how a non-canonical writer's layout is resolved at bind time -->

# Schema and profile

The kernel binds against a *logical* schema — the profile — and any physical layout that maps onto it works. When the writer is under the project's control the canonical layout below is recommended; when it is not, the profile names the relation and the column roles, resolved fresh at bind time so widened types (RawDuck evolves columns) are picked up.

## Design decisions

Series get an identity column: a `ts_series` dimension table holding the full label set once, and a 64-bit `series_id` on every sample row, so grouping is an integer hash and label matching is a semi-join on a small table — the split ClickHouse's TimeSeries engine makes, and the cost cerberus's label-map tower pays per row otherwise. Histograms are typed structs, not parallel arrays. Nested things (exemplars, span events, span links) are child tables with keys rather than `LIST<STRUCT>`, per the TUM "nested Parquet is flat" result. Physical sort order is the index — DuckDB has zone maps and nothing else — so metrics sort by `(metric_name, series_id, ts)`, with `metric_name` deliberately denormalised onto sample rows because zone maps only work on the table being scanned.

## Canonical metrics layout (abridged)

    CREATE TABLE ts_series (
      series_id UBIGINT PRIMARY KEY, metric_name VARCHAR NOT NULL, service_name VARCHAR,
      labels MAP(VARCHAR, VARCHAR), resource MAP(VARCHAR, VARCHAR), unit VARCHAR, description VARCHAR);

    CREATE TABLE ts_samples (            -- gauges and sums; physical order (metric_name, series_id, ts)
      metric_name VARCHAR NOT NULL, series_id UBIGINT NOT NULL, ts TIMESTAMP NOT NULL,
      value DOUBLE NOT NULL,             -- NaN with the stale payload = stale marker
      start_ts TIMESTAMP, kind UTINYINT, monotonic BOOLEAN, temporality UTINYINT);

    CREATE TABLE ts_histograms (         -- classic
      metric_name VARCHAR, series_id UBIGINT, ts TIMESTAMP, start_ts TIMESTAMP,
      h STRUCT(count UBIGINT, sum DOUBLE, min DOUBLE, max DOUBLE, bounds DOUBLE[], counts UBIGINT[]));

    CREATE TABLE ts_exp_histograms (     -- native / exponential; scale = OTel scale = Prometheus schema
      metric_name VARCHAR, series_id UBIGINT, ts TIMESTAMP, start_ts TIMESTAMP,
      h STRUCT(count UBIGINT, sum DOUBLE, scale TINYINT, zero_count UBIGINT, zero_threshold DOUBLE,
               pos_offset INTEGER, pos_counts UBIGINT[], neg_offset INTEGER, neg_counts UBIGINT[], reset_hint UTINYINT));

    CREATE TABLE ts_exemplars (series_id UBIGINT, ts TIMESTAMP, exemplar_ts TIMESTAMP, value DOUBLE,
      trace_id UHUGEINT, span_id UBIGINT, labels MAP(VARCHAR, VARCHAR));

Spans and logs follow the same rules — `trace_id` as `UHUGEINT`, Tempo's `nested_left / nested_right / depth` per span populated by a finalisation pass with a recursive-CTE fallback, a `lg_streams` identity table, Loki's structured-metadata/attributes split kept because it is a pushdown-eligibility boundary — but they are consumer-side concerns and belong in the consumer projects' own docs, not here. The kernel's own interest in them is only that a span duration or a log-line count arrives as `(ts, value)`.

## The profile

    CALL ts_profile_set('default', {
      samples:        {rel: 'ts_samples', ts: 'ts', value: 'value', metric: 'metric_name',
                       series: 'series_id', start_ts: 'start_ts', kind: 'kind', monotonic: 'monotonic'},
      histograms:     {rel: 'ts_histograms',     hist: 'h'},
      exp_histograms: {rel: 'ts_exp_histograms', hist: 'h'},
      series:         {rel: 'ts_series', id: 'series_id', labels: 'labels', resource: 'resource',
                       promoted: ['service_name']},
      lookback:       INTERVAL 5 MINUTE,        -- per profile, per resolution
      stale:          NULL,                      -- or a boolean column for OTLP/JSON writers with no NaN marker
      tie_break:      'total'                    -- the declared duplicate-timestamp order; no reference defines one
    });

A profile for a stock OTel-exporter-shaped Parquet dump has no `series` relation and points `labels` at the sample row's map; the kernel then hashes labels per row, slower but correct. A RawDuck profile points at the shredded tables and lists the promoted columns it finds at bind time. Missing roles degrade features rather than fail: no `start_ts` means no delta-temporality support; no `series` means per-row hashing.
