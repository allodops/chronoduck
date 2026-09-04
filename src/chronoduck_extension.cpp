#define DUCKDB_EXTENSION_MAIN

#include "chronoduck_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

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

} // namespace

static void LoadInternal(ExtensionLoader &loader) {
	// Register the niladic version function
	auto chronoduck_version_scalar_function =
	    ScalarFunction("chronoduck_version", {}, LogicalType::VARCHAR, ChronoduckVersionScalarFun);
	loader.RegisterFunction(chronoduck_version_scalar_function);
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
