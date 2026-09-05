-- test/partners/rawduck/scan-bound-absent.sql
--
-- T2.3 (#41): on a table-function/storage-partner source, the scan-bound
-- pushdown rule declines to push -- docs/design/architecture.md's own line:
-- "on a table-function source ... the bound is asserted absent and the
-- residual filter kept." test/sql/scan_bound_pushdown.test already proves
-- this against a range()-based stand-in on the merge gate (that file's own
-- comment explains why a real partner isn't part of that lane); this file
-- proves the same thing against the real partner. Deliberately NOT named
-- `*.test` -- see this directory's own smoke.sql header comment for why
-- (DuckDB's own sqllogictest runner would otherwise auto-discover and
-- misparse it).

-- A minimal RawMergeTree store, the same shape smoke.sql already
-- establishes works with chronoduck's own ts_rate.
ATTACH 'rawduck:rawduck_scan_bound_absent.db' AS raw;

INSERT INTO raw.ingest.metrics VALUES
    ('{"ts": "2024-01-01T00:00:00", "v": 10.0}'),
    ('{"ts": "2024-01-01T00:00:01", "v": 20.0}'),
    ('{"ts": "2024-01-01T00:00:02", "v": 40.0}');

PRAGMA enable_profiling='json';
PRAGMA profiling_output='scan_bound_absent.json';

SELECT ts_rate(ts, v, TIMESTAMP '2024-01-01 00:00:02', TIMESTAMP '2024-01-01 00:00:02', 1000000, 2000000)[1]
FROM raw.metrics;

PRAGMA disable_profiling;

-- No "Filters" key at all should reach the RawDuck-backed Get's own
-- extra_info -- the same JSON keys test/sql/scan_bound_pushdown.test reads,
-- pinned in .github/ci-lanes.json's own "build-test" lane entry. This
-- harness (scripts/partners/rawduck-test.py) only detects failure via a
-- non-zero exit or an "Error:"-prefixed line, never a printed boolean, so
-- the assertion itself must raise on failure rather than just report one.
SELECT CASE WHEN NOT contains(content, '"Filters"') THEN 'ok'
            ELSE error('scan-bound pushdown must not push a filter onto a RawDuck-backed Get')
       END
FROM read_text('scan_bound_absent.json');
