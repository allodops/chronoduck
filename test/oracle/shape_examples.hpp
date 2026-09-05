// shape_examples.hpp — the ShapeID roster's own deterministic worked
// examples (docs/testing/layers.md's L3 row: "an exact ShapeID roster with
// one deterministic live example per shape"). A ShapeID is `name/edge_mode/
// domain` (`docs/testing/registry-and-fixtures.md`: "the property roster —
// each name x edge mode x value domain is a ShapeID"), with any leading
// `ts_` stripped the same way `scripts/hygiene/registry-roster-closure.py`
// already matches a fixture's `function:` field against a registry row.
//
// This file names no expected NUMBER independently derived here — each
// example's `expected` field is the same value
// `test/fixtures/rate/*.yaml` already carries (cited by filename in each
// entry's own comment), so the ShapeID roster's worked example is grounded
// in the same hand-derived arithmetic the L2 fixture roster already reviews,
// rather than a second, silently-drifting copy of it. Only the DATA is
// shared; the evaluation is not — `test/kernel/oracle_sweep_test.cpp` runs
// each entry through both this issue's own from-scratch `rate_oracle.hpp`
// AND the real `ts_rate` composition, and checks both against `expected`
// with the one real comparator (`src/kernel/comparator.hpp`), which this
// file (living under `test/oracle/`, fenced from `src/`) cannot import
// itself.
//
// `scripts/hygiene/shape-roster.py` (T7, docs/testing/rules.md) reads the
// `// ShapeID: <id>` citation directly above each entry below as this
// roster's own "current" set, ratcheted against
// `test/oracle/shape-roster.json` exactly the way
// `scripts/hygiene/kernel-fixture-loader.py` ratchets `test/fixtures/
// roster.json` against the fixtures it finds — REGRESSED/VANISHED/
// ARRIVED-FAILING/UNRECORDED are all fatal there, per Article V.4.
#pragma once

#include "series.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace chronoduck::oracle {

struct ShapeExample {
	std::string shape_id;
	std::vector<OracleSample> samples;
	OracleGrid grid;
	int64_t window;
	double expected;
};

// ShapeID: rate/EXTRAPOLATE/COUNTER
//
// test/fixtures/rate/edge-boundary.yaml verbatim: the left-open,
// right-closed window rule. A decoy at exactly the excluded lower edge
// (t=100, v=999 — a value that would trigger a spurious reset if ever
// wrongly admitted) is followed by the two real samples (101, 10) and
// (200, 40); window (100, 200] excludes the decoy, delta = 40 - 10 = 30 over
// S = 200 - 101 = 99 (both edge gaps under the 1.1x threshold), factor =
// 100/99, rate = 30/99 = 0.30303030303030304.
inline ShapeExample RateExtrapolateCounterExample() {
	ShapeExample ex;
	ex.shape_id = "rate/EXTRAPOLATE/COUNTER";
	ex.samples = {
	    {100, 999.0, false, 0},
	    {101, 10.0, false, 0},
	    {200, 40.0, false, 0},
	};
	ex.grid = OracleGrid {200, 200, 1};
	ex.window = 100;
	ex.expected = 0.30303030303030304;
	return ex;
}

// The full ShapeID roster this issue's registry can express: a single-row
// vector, since `src/kernel/registry.def` currently declares exactly one
// (name, edge_mode, domain) combination with a real domain. Registering
// another combination needs its own entry here plus a `// ShapeID:`
// citation, or `scripts/hygiene/shape-roster.py` reports it as
// ARRIVED-FAILING).
inline std::vector<ShapeExample> ShapeIdRoster() {
	return {RateExtrapolateCounterExample()};
}

} // namespace chronoduck::oracle
