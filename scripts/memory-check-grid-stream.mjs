#!/usr/bin/env bun
// Issue #40's second acceptance criterion — "resident state <= threads *
// window * c on the 30-day/1s/5-minute-window shape" — measured through the
// real operator (`PhysicalGridStream`, the `ts_rate(...)` table-function
// overload), not just `src/kernel/grid_stream.hpp`'s own primitive (that one
// is `test/kernel/grid_stream_test.cpp`'s `TestResidencyBoundedByWindowNotRange`,
// which already proves the primitive's `resident_size()` peaks at a small
// constant multiple of the window, with zero DuckDB overhead in the way).
//
// This script is the operator-level counterpart docs/testing/memory.md calls
// for: process peak RSS (from `/usr/bin/time -v`'s "Maximum resident set
// size", the same `/proc`-sourced ceiling number that document names) on a
// grid/step/window shape held fixed while *only* series count varies by a
// large factor at constant total row count — the same experiment the PR body
// for issue #40 described in prose (24 vs. 2,400 series, ~62M total samples,
// 5.22 GB vs. 4.07 GB), now checked in and asserting instead of a one-off
// manual run.
//
// The 30-day range itself is scaled down to 1 hour: `grid_step` (1s) and
// `window` (5 min) are the real shape's own values, but the *range* only
// matters here because `PhysicalGridStream::ProcessOneBin` builds one
// grid.count()-sized RatePoint output vector per active series (the
// unavoidable cost of the operator's own `(series_id, values LIST(DOUBLE))`
// output shape, not window-fold state) — at the full 30-day range that
// output-materialization cost would swamp the window-residency signal this
// script exists to isolate. Series count is pushed past the PR's own 2,400
// up to 20,000 (closer to docs/testing/memory.md's actual "10k-series"
// target) precisely because that signal is small (a `GridStream`'s own
// resident buffer is a few KB) and needs enough series for a real regression
// to be visible over the sort/materialization baseline that scales with
// total row count, not series count. `SET threads TO 1` is deliberate: the
// leak this script exists to catch (see below) is `thread_local`, and a
// single thread makes the whole run's worth of series-boundary transitions
// accumulate in one place instead of splitting across threads.
//
// Self-check calibration (docs/testing/memory.md's "asserts the corresponding
// sentinel goes red" — run by hand against a temporary mutant (#45 owns
// wiring this self-check into a real CI lane), not part of this script,
// since it edits src/chronoduck_extension.cpp):
// commenting out `active_stream`'s reassignment in `ProcessOneBin`'s
// `start_series` (i.e. leaking the old `GridStream` instead of releasing it
// at the series boundary — the exact regression this document names) moved
// the high-series/low-series ratio from ~1.01x (this script's passing
// baseline) to ~1.16x on this exact shape. MAX_GROWTH_RATIO below sits
// between those two, with slack on both sides for run-to-run noise.
import { mkdtempSync, existsSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const DUCKDB_BIN = "./build/release/duckdb";
const EXTENSION_PATH = "./build/release/extension/chronoduck/chronoduck.duckdb_extension";
const TIME_BIN = "/usr/bin/time";

const GRID_STEP_US = 1_000_000; // 1s, the shape's own grid resolution
const WINDOW_US = 300_000_000; // 5 minutes, the shape's own window
const GRID_RANGE_S = 3600; // scaled down from 30 days — see header comment
const TOTAL_ROWS = 20_000_000; // held constant across both configs
const LOW_SERIES = 24; // the PR's own "low" series count
const HIGH_SERIES = 20_000; // pushed toward the documented ~10k-series target
const MAX_GROWTH_RATIO = 1.1; // generous slack above the ~1.01x measured baseline,
// well below the ~1.16x measured for the named leak mutant (see header)

function buildSql(seriesCount) {
  return `
LOAD '${EXTENSION_PATH}';
SET threads TO 1;
CREATE TEMP TABLE samples AS
SELECT
    (i % ${seriesCount})::UBIGINT AS series_id,
    TIMESTAMP '2024-01-01 00:00:00' + INTERVAL ((i // ${seriesCount})) SECOND AS ts,
    (i % 97)::DOUBLE AS value
FROM generate_series(0, ${TOTAL_ROWS - 1}) AS t(i);
SELECT count(*) FROM ts_rate(
    (SELECT series_id, ts, value FROM samples),
    TIMESTAMP '2024-01-01 00:00:00',
    TIMESTAMP '2024-01-01 00:00:00' + INTERVAL (${GRID_RANGE_S}) SECOND,
    ${GRID_STEP_US}, ${WINDOW_US}
);
`;
}

async function run(cmd, options) {
  const proc = Bun.spawn(cmd, { stdout: "pipe", stderr: "pipe", ...options });
  const [out, err, code] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
    proc.exited,
  ]);
  return { out, err, code };
}

// Runs one config through `/usr/bin/time -v` and returns its peak RSS in KB,
// parsed from "Maximum resident set size (kbytes): N" on stderr — the same
// process-peak-RSS number docs/testing/memory.md calls for.
async function measurePeakKb(sqlPath) {
  const { out, err, code } = await run([TIME_BIN, "-v", DUCKDB_BIN, "-unsigned"], {
    stdin: Bun.file(sqlPath),
  });
  if (code !== 0) {
    throw new Error(`duckdb exited ${code}\nstdout:\n${out}\nstderr:\n${err}`);
  }
  const match = err.match(/Maximum resident set size \(kbytes\):\s*(\d+)/);
  if (!match) {
    throw new Error(`could not find "Maximum resident set size" in /usr/bin/time -v output:\n${err}`);
  }
  return Number.parseInt(match[1], 10);
}

if (!existsSync(DUCKDB_BIN) || !existsSync(EXTENSION_PATH)) {
  console.error("memory-check-grid-stream: FAIL");
  console.error(`  ${DUCKDB_BIN} / ${EXTENSION_PATH} not found — run \`make release\` first`);
  process.exit(1);
}
if (!existsSync(TIME_BIN)) {
  console.error("memory-check-grid-stream: FAIL");
  console.error(`  ${TIME_BIN} not found (GNU time with -v support is required)`);
  process.exit(1);
}

const tmp = mkdtempSync(join(tmpdir(), "memory-check-grid-stream-"));
const lowSqlPath = join(tmp, "low.sql");
const highSqlPath = join(tmp, "high.sql");
writeFileSync(lowSqlPath, buildSql(LOW_SERIES));
writeFileSync(highSqlPath, buildSql(HIGH_SERIES));

console.error(
  `memory-check-grid-stream: running ${LOW_SERIES}-series config (${TOTAL_ROWS} total rows, ` +
    `1h/1s/5min shape)...`
);
const lowKb = await measurePeakKb(lowSqlPath);
console.error(`memory-check-grid-stream: ${LOW_SERIES} series -> peak RSS ${lowKb} KB`);

console.error(
  `memory-check-grid-stream: running ${HIGH_SERIES}-series config (${TOTAL_ROWS} total rows, ` +
    `1h/1s/5min shape)...`
);
const highKb = await measurePeakKb(highSqlPath);
console.error(`memory-check-grid-stream: ${HIGH_SERIES} series -> peak RSS ${highKb} KB`);

const ratio = highKb / lowKb;
console.error(
  `memory-check-grid-stream: ${HIGH_SERIES / LOW_SERIES}x more series -> ` +
    `${ratio.toFixed(3)}x peak RSS (bound: <= ${MAX_GROWTH_RATIO}x)`
);

if (ratio > MAX_GROWTH_RATIO) {
  console.error("memory-check-grid-stream: FAIL");
  console.error(
    `  peak RSS grew ${ratio.toFixed(3)}x for a ${HIGH_SERIES / LOW_SERIES}x increase in series count — ` +
      `resident state is tracking series count, not just threads * window (docs/testing/memory.md, ` +
      `issue #40 acceptance criterion 2)`
  );
  process.exit(1);
}
console.log("memory-check-grid-stream: PASS");
