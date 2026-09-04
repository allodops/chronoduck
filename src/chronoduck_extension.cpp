#define DUCKDB_EXTENSION_MAIN

#include "chronoduck_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include <limits>

namespace duckdb {

namespace {

// The version string embedded at build time via the EXT_VERSION_CHRONODUCK
// compile-time define (set by duckdb_extension_generate_version, derived
// from `git describe` on this repo — see
// duckdb/extension/extension_build_tools.cmake:duckdb_extension_load).
// Falls back to a literal when that define is absent, so the function stays
// well-defined outside that build path too.
std::string ChronoduckVersionString() {
#ifdef EXT_VERSION_CHRONODUCK
	return EXT_VERSION_CHRONODUCK;
#else
	return "0.0.1";
#endif
}

void ChronoduckVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	Value val(ChronoduckVersionString());
	result.Reference(val);
}

// Genuine floor division, not C++'s truncate-toward-zero `/` — required so
// the Grid primitive's invariant (at(index_of(t)) <= t < at(index_of(t)+1),
// docs/testing/primitives.md's Grid row) holds for t before the grid start
// too, where the numerator is negative. The numerator is __int128_t (not
// int64_t) because t.value - grid_start.value, computed by the caller, can
// itself overflow int64_t: DuckDB's representable TIMESTAMP range spans
// about [-290308-12-22, 294247-01-01], so two legal, in-range timestamps
// can be up to ~1.85e19 microseconds apart — beyond int64_t's ~9.22e18 max.
// __int128_t is a GCC/Clang extension, safe here since this project's CI
// targets ubuntu-latest exclusively (see .github/workflows/ci.yml).
__int128_t FloorDiv(__int128_t numerator, int64_t denominator) {
	__int128_t quotient = numerator / denominator;
	__int128_t remainder = numerator % denominator;
	if (remainder != 0 && ((remainder < 0) != (denominator < 0))) {
		quotient--;
	}
	return quotient;
}

// ts_grid_index(t, start, step) = floor((t - start) / step): the index of
// the grid point at or before t on a Grid{start, step} — see
// docs/design/primitives.md's Tier 1 row. Deliberately unclamped: a
// negative index for t < start is correct, not an error, since the same
// invariant covers that case.
void TsGridIndexScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	TernaryExecutor::Execute<timestamp_t, timestamp_t, int64_t, int64_t>(
	    args.data[0], args.data[1], args.data[2], result, args.size(),
	    [](timestamp_t t, timestamp_t grid_start, int64_t step) -> int64_t {
		    if (step <= 0) {
			    throw InvalidInputException("ts_grid_index: step must be positive, got %lld", (long long)step);
		    }
		    // Widen to __int128_t before subtracting: t.value - grid_start.value
		    // can overflow int64_t for legal, in-range TIMESTAMP extremes (see
		    // FloorDiv above).
		    __int128_t offset = static_cast<__int128_t>(t.value) - static_cast<__int128_t>(grid_start.value);
		    __int128_t index = FloorDiv(offset, step);
		    // The result narrows back to int64_t (ts_grid_index returns BIGINT).
		    // That narrowing itself can't be assumed safe: with a small enough
		    // step relative to an extreme offset, the mathematically correct
		    // index doesn't fit in int64_t either, so detect that and raise a
		    // clear error rather than silently wrapping — the same posture as
		    // the "step must be positive" check above.
		    if (index < std::numeric_limits<int64_t>::min() || index > std::numeric_limits<int64_t>::max()) {
			    throw InvalidInputException("ts_grid_index: computed index overflows BIGINT for these inputs "
			                                 "(t, start too far apart relative to step)");
		    }
		    return static_cast<int64_t>(index);
	    });
}

} // namespace

static void LoadInternal(ExtensionLoader &loader) {
	// Register the niladic version function
	auto chronoduck_version_scalar_function =
	    ScalarFunction("chronoduck_version", {}, LogicalType::VARCHAR, ChronoduckVersionScalarFun);
	loader.RegisterFunction(chronoduck_version_scalar_function);

	// Canary: the first real function through claim -> branch -> implement ->
	// PR -> review -> merge loop. Not yet in registry.def (#26 moves it
	// there — Article V.1 does not apply until that file exists).
	auto ts_grid_index_scalar_function =
	    ScalarFunction("ts_grid_index", {LogicalType::TIMESTAMP, LogicalType::TIMESTAMP, LogicalType::BIGINT},
	                   LogicalType::BIGINT, TsGridIndexScalarFun);
	loader.RegisterFunction(ts_grid_index_scalar_function);
}

void ChronoduckExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string ChronoduckExtension::Name() {
	return "chronoduck";
}

std::string ChronoduckExtension::Version() const {
	return ChronoduckVersionString();
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(chronoduck, loader) {
	duckdb::LoadInternal(loader);
}
}
