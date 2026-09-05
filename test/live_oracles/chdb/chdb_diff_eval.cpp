// chdb_diff_eval.cpp — the L6a chDB leg (#43, T2.5): one invocation runs one
// `rate` fixture against chDB's own native `timeSeriesRateToGrid` aggregate
// and compares its array to the kernel's under `src/kernel/comparator.hpp`'s
// own `equal_values`, applying `src/kernel/chdb_divergence.hpp`'s
// `ChdbDivergence` enum. Compiled once by `scripts/live-oracles/chdb-differential.py`
// with a bare `g++` against the vendored `libchdb.so`
// (`scripts/live-oracles/chdb-fetch.py`) — the same "wire protocol over
// stdin, bare g++, real kernel headers" shape `test/kernel/rate_fixture_loader.cpp`
// already established for the L2 leg, extended here with one more
// dependency (chDB) on one side of the comparison.
//
// The kernel side of the comparison is `test/kernel/rate_fixture_eval.hpp`'s
// `EvaluateRate` — the same composition L2's own loader already certifies
// against `ts_rate`'s live SQL aggregate (`ts_rate`'s registry row declares
// `D0`: bit-identical regardless of path, `src/kernel/registry.def:TS_FN:`
// `ts_rate,            RANGE, RAW_WINDOW, D0`), rather than re-querying
// `ts_rate` through a second spawned DuckDB process. This keeps the entire
// kernel-vs-chDB comparison inside one sanitizer-decidable binary and avoids
// re-deriving the tick-to-TIMESTAMP translation `test/sql/ts_rate.test`
// already solves a second time.
//
// Wire format (stdin), one fixture:
//   GRID <start> <end> <step>
//   WINDOW <window>
//   NSAMPLES <n>
//   <t> <v>                         (n lines; no `st` — a fixture with a
//                                    start-timestamp sample has no chDB
//                                    signature to run at all, so the driver
//                                    never invokes this program for one; see
//                                    docs/testing/live-oracles.md's ✗-by-shape
//                                    roster gap, not a divergence)
//   DIVERGENCE <NONE|DUP_TS_KEEPS_MAX|NULL_FOR_TOO_FEW>
//
// Output: one `POINT <i> <PASS|FAIL> <kernel-or-NULL> <chdb-or-NULL> <divergence>`
// line per grid point, then `RESULT PASS`/`RESULT FAIL` (malformed input or a
// chDB query error is `RESULT ERROR <message>`, exit 2). Exit code mirrors
// PASS/FAIL: 0/1.
//
// Sanitizers: libchdb.so bundles ClickHouse's own allocator, which crashes
// immediately inside `chdb_connect` when AddressSanitizer's malloc
// interception is also active in the process — confirmed empirically while
// building this file (`munmap_chunk(): invalid pointer`, before any query
// even runs; UBSan alone does not trigger it). This program is therefore
// compiled by its own dedicated bare-g++ invocation, the same way
// `rate_fixture_loader.cpp` already sits outside the CMake/sanitizer build
// matrix (`make debug`/`make relassert`/`THREADSAN=1 make debug` never
// touch `test/live_oracles/`) — it must stay that way; folding this file
// into a sanitized build target breaks the L6a lane outright, not just
// weakens it. Independent of sanitizers, `main` below calls `_exit()`
// rather than `return` after closing the chDB connection: chDB's own C API
// contract is one connection per process, and `_exit()` skips whatever
// static-destruction order a long-running embedded native engine might not
// have exercised for a single-shot CLI use.
#include "../../../src/kernel/chdb_divergence.hpp"
#include "../../../src/kernel/comparator.hpp"
#include "../../kernel/rate_fixture_eval.hpp"
#include "chdb.h"

#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

using namespace chronoduck;
using namespace chronoduck::fixtures;

namespace {

[[noreturn]] void FailInput(const std::string &what) {
	std::cout << "RESULT ERROR malformed input: " << what << "\n";
	std::cout.flush();
	_exit(2);
}

[[noreturn]] void FailQuery(chdb_connection *conn, const std::string &what) {
	std::cout << "RESULT ERROR " << what << "\n";
	std::cout.flush();
	if (conn) {
		chdb_close_conn(conn);
	}
	_exit(2);
}

std::string ReadToken(std::istream &in, const char *expected_keyword) {
	std::string tok;
	if (!(in >> tok) || tok != expected_keyword) {
		FailInput(std::string("expected \"") + expected_keyword + "\"");
	}
	return tok;
}

int64_t ReadInt(std::istream &in, const char *what) {
	int64_t v;
	if (!(in >> v)) {
		FailInput(std::string("expected an integer for ") + what);
	}
	return v;
}

const char *DivergenceName(std::optional<ChdbDivergence> d) {
	if (!d.has_value())
		return "NONE";
	switch (*d) {
	case ChdbDivergence::DUP_TS_KEEPS_MAX:
		return "DUP_TS_KEEPS_MAX";
	case ChdbDivergence::NULL_FOR_TOO_FEW:
		return "NULL_FOR_TOO_FEW";
	}
	return "NONE";
}

std::optional<ChdbDivergence> ParseDivergence(const std::string &tok) {
	if (tok == "DUP_TS_KEEPS_MAX")
		return ChdbDivergence::DUP_TS_KEEPS_MAX;
	if (tok == "NULL_FOR_TOO_FEW")
		return ChdbDivergence::NULL_FOR_TOO_FEW;
	if (tok == "NONE")
		return std::nullopt;
	FailInput("unrecognised DIVERGENCE token \"" + tok + "\"");
}

// The per-fixture SQL template: chDB's own `timeSeriesRateToGrid`, called in
// the scalar-array form `docs/testing/live-oracles.md`'s example and
// ClickHouse's own reference documentation both use (no FROM/table needed —
// the aggregate evaluates directly over its array arguments). Every sample
// value is an explicit `CAST(... AS Float64)` — the family "computes in the
// input's type", so a harness bug here would silently run the comparison in
// Float32 instead of a declared divergence
// (`docs/testing/live-oracles.md:chdb-float32-leg:` `a Float32 leg would be a harness bug, not a divergence`).
std::string BuildQuery(int64_t start, int64_t end, int64_t step, int64_t window,
                       const std::vector<RawSample> &samples) {
	std::ostringstream ts;
	std::ostringstream vs;
	ts << "[";
	vs << "[";
	for (std::size_t i = 0; i < samples.size(); i++) {
		if (i != 0) {
			ts << ", ";
			vs << ", ";
		}
		ts << "toDateTime64(" << samples[i].t << ", 6)";
		vs << "CAST(" << std::setprecision(17) << samples[i].v << " AS Float64)";
	}
	ts << "]::Array(DateTime64(6))";
	vs << "]::Array(Float64)";

	std::ostringstream sql;
	sql << "SELECT timeSeriesRateToGrid(" << start << ", " << end << ", " << step << ", " << window << ")(" << ts.str()
	    << ", " << vs.str() << ")";
	return sql.str();
}

// `L1 test on the SQL template`: `docs/testing/live-oracles.md`'s own
// requirement that the harness is checked to pass Float64, not the family's
// documented Float32 example. A mechanical string check on the template
// this program actually issues, run once at startup rather than left to be
// eyeballed in a code review.
void AssertTemplateIsFloat64() {
	std::vector<RawSample> one = {{0, 1.5, false, 0}};
	std::string q = BuildQuery(0, 0, 1, 1, one);
	if (q.find("CAST(1.5 AS Float64)") == std::string::npos) {
		std::cout << "RESULT ERROR L1 self-check failed: SQL template does not CAST sample values to Float64: " << q
		          << "\n";
		_exit(2);
	}
}

// Parses chDB's `CSV` response for one `Array(Nullable(Float64))` row, e.g.
// `"[0.30303030303030304]"` or `"[NULL,NULL,0,0.5]"` (CSV quotes an
// Array-typed field as a single string column). Tolerant of the surrounding
// quotes and trailing newline CSV emits; the bracket-to-bracket substring is
// exactly the array literal ClickHouse serialised, so a plain comma split
// suffices — no fixture value or chDB output this family ever emits embeds a
// comma inside one element.
std::vector<std::optional<double>> ParseChdbArray(const std::string &csv) {
	std::size_t open = csv.find('[');
	std::size_t close = csv.rfind(']');
	if (open == std::string::npos || close == std::string::npos || close <= open) {
		FailInput("could not find an array in chDB's response: " + csv);
	}
	std::string inner = csv.substr(open + 1, close - open - 1);

	std::vector<std::optional<double>> out;
	std::size_t start = 0;
	while (start <= inner.size()) {
		std::size_t comma = inner.find(',', start);
		std::string tok = inner.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		// trim whitespace
		std::size_t a = tok.find_first_not_of(" \t");
		std::size_t b = tok.find_last_not_of(" \t");
		tok = (a == std::string::npos) ? "" : tok.substr(a, b - a + 1);
		if (tok == "NULL" || tok.empty()) {
			out.push_back(std::nullopt);
		} else {
			out.push_back(std::stod(tok));
		}
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	return out;
}

} // namespace

int main() {
	ReadToken(std::cin, "GRID");
	int64_t start = ReadInt(std::cin, "grid.start");
	int64_t end = ReadInt(std::cin, "grid.end");
	int64_t step = ReadInt(std::cin, "grid.step");
	Grid grid {start, end, step};

	ReadToken(std::cin, "WINDOW");
	int64_t window = ReadInt(std::cin, "window");

	ReadToken(std::cin, "NSAMPLES");
	int64_t n_samples = ReadInt(std::cin, "NSAMPLES");
	if (n_samples < 0) {
		FailInput("NSAMPLES must be >= 0");
	}
	std::vector<RawSample> samples;
	samples.reserve(static_cast<std::size_t>(n_samples));
	for (int64_t i = 0; i < n_samples; i++) {
		RawSample s;
		s.t = ReadInt(std::cin, "sample.t");
		double v;
		if (!(std::cin >> v)) {
			FailInput("expected sample.v");
		}
		s.v = v;
		samples.push_back(s);
	}

	ReadToken(std::cin, "DIVERGENCE");
	std::string divergence_tok;
	if (!(std::cin >> divergence_tok)) {
		FailInput("expected a DIVERGENCE token");
	}
	std::optional<ChdbDivergence> divergence = ParseDivergence(divergence_tok);

	AssertTemplateIsFloat64();

	std::vector<RatePoint> kernel = EvaluateRate(samples, grid, window);

	std::string sql = BuildQuery(start, end, step, window, samples);

	char prog_name[] = "chdb";
	char *argv[] = {prog_name};
	chdb_connection *conn = chdb_connect(1, argv);
	if (!conn) {
		FailQuery(nullptr, "chdb_connect returned null");
	}

	chdb_result *set_result =
	    chdb_query(*conn, "SET allow_experimental_time_series_aggregate_functions = 1", "TabSeparated");
	const char *set_err = set_result ? chdb_result_error(set_result) : "chdb_query returned null";
	if (set_err) {
		std::string msg = set_err;
		if (set_result)
			chdb_destroy_query_result(set_result);
		FailQuery(conn, "failed to enable allow_experimental_time_series_aggregate_functions: " + msg);
	}
	chdb_destroy_query_result(set_result);

	chdb_result *result = chdb_query(*conn, sql.c_str(), "CSV");
	if (!result) {
		FailQuery(conn, "chdb_query returned null for: " + sql);
	}
	const char *query_err = chdb_result_error(result);
	if (query_err) {
		std::string msg = query_err;
		chdb_destroy_query_result(result);
		FailQuery(conn, "chDB query failed: " + msg + " (SQL: " + sql + ")");
	}
	std::string csv(chdb_result_buffer(result), chdb_result_length(result));
	chdb_destroy_query_result(result);

	std::vector<std::optional<double>> chdb_values = ParseChdbArray(csv);

	if (static_cast<int64_t>(chdb_values.size()) != grid.count() ||
	    static_cast<int64_t>(kernel.size()) != grid.count()) {
		FailQuery(conn, "grid point count mismatch: kernel=" + std::to_string(kernel.size()) +
		                    " chdb=" + std::to_string(chdb_values.size()) + " grid=" + std::to_string(grid.count()));
	}

	bool all_pass = true;
	for (std::size_t i = 0; i < kernel.size(); i++) {
		const RatePoint &k = kernel[i];
		const std::optional<double> &c = chdb_values[i];

		bool pass;
		if (k.has_value != c.has_value()) {
			pass = false; // one side has a value, the other doesn't: never a pass
		} else if (!k.has_value) {
			pass = true; // both null
		} else {
			pass = equal_values(k.value, *c, k.scale);
		}
		all_pass = all_pass && pass;

		std::printf("POINT %zu %s %s %s %s\n", i, pass ? "PASS" : "FAIL",
		            k.has_value ? std::to_string(k.value).c_str() : "NULL",
		            c.has_value() ? std::to_string(*c).c_str() : "NULL", DivergenceName(divergence));

		if (!pass && divergence.has_value()) {
			std::fprintf(stderr,
			             "chdb_diff_eval: point %zu declares ChdbDivergence::%s as an asserted contract "
			             "(the two engines were expected to agree, not to need excusing) but did not match\n",
			             i, DivergenceName(divergence));
		}
	}

	std::printf("RESULT %s\n", all_pass ? "PASS" : "FAIL");
	std::fflush(stdout);

	chdb_close_conn(conn);
	_exit(all_pass ? 0 : 1);
}
