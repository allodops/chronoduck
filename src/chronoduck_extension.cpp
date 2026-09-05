#define DUCKDB_EXTENSION_MAIN

#include "chronoduck_extension.hpp"
#include "kernel/counter_fold.hpp"
#include "kernel/extrapolate.hpp"
#include "kernel/grid.hpp"
#include "kernel/registry_types.hpp"
#include "kernel/sample_buffer.hpp"
#include "kernel/window_walk.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include <limits>
#include <map>
#include <utility>
#include <vector>

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

using chronoduck::CounterFoldSummary;
using chronoduck::CounterSample;
using chronoduck::ExtrapolateResult;
using chronoduck::Grid;
using chronoduck::Sample;
using chronoduck::SampleBuffer;
using chronoduck::SampleSpan;
using chronoduck::WindowRange;

// bind_grid_args (Tier 6's own row, `docs/testing/primitives.md:bind_grid_args-row:`
// `Constant vs non-constant arguments`): validates one bind-time (start, end,
// step) triple and turns `Grid`'s own construction errors
// (`src/kernel/grid.hpp:Grid:` `Grid: step must be positive`) into a
// `BinderException` carrying the same message — one error catalogue shared
// by `ts_grid`'s table-function bind and `ts_rate`'s aggregate bind below,
// rather than two copies of the same three checks.
Grid BindGridArgs(int64_t start, int64_t end, int64_t step) {
	try {
		return Grid(start, end, step);
	} catch (const std::invalid_argument &ex) {
		throw BinderException(ex.what());
	}
}

// Requires bind-time argument `idx` of an aggregate call to be foldable —
// the grid arguments are bind data, never per-row columns
// (`docs/design/architecture.md:where-it-plugs-in:` `The grid is bind
// data`) — and folds it to a `Value`. The "constant vs non-constant
// arguments" row of `bind_grid_args`'s error catalogue.
Value RequireConstantArg(ClientContext &context, vector<unique_ptr<Expression>> &arguments, idx_t idx,
                         const char *name) {
	Expression &expr = *arguments[idx];
	if (!expr.IsFoldable()) {
		throw BinderException("ts_rate: %s must be a constant expression", name);
	}
	Value value = ExpressionExecutor::EvaluateScalar(context, expr);
	if (value.IsNull()) {
		throw BinderException("ts_rate: %s must not be NULL", name);
	}
	return value;
}

//===--------------------------------------------------------------------===//
// ts_grid — GRID family table function: (start, end, step) -> one TIMESTAMP
// row per grid point.
//===--------------------------------------------------------------------===//

struct TsGridBindData : public TableFunctionData {
	explicit TsGridBindData(Grid grid_p) : grid(grid_p) {
	}
	Grid grid;
};

struct TsGridGlobalState : public GlobalTableFunctionState {
	idx_t current_idx = 0;
};

unique_ptr<FunctionData> TsGridBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
	return_types.push_back(LogicalType::TIMESTAMP);
	names.push_back("ts");

	auto &inputs = input.inputs;
	if (inputs[0].IsNull() || inputs[1].IsNull() || inputs[2].IsNull()) {
		throw BinderException("ts_grid: start, end and step must not be NULL");
	}
	int64_t start = inputs[0].GetValue<timestamp_t>().value;
	int64_t end = inputs[1].GetValue<timestamp_t>().value;
	int64_t step = inputs[2].GetValue<int64_t>();
	return make_uniq<TsGridBindData>(BindGridArgs(start, end, step));
}

unique_ptr<GlobalTableFunctionState> TsGridInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<TsGridGlobalState>();
}

void TsGridFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<TsGridBindData>();
	auto &state = data_p.global_state->Cast<TsGridGlobalState>();

	auto result_data = FlatVector::GetData<timestamp_t>(output.data[0]);
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.current_idx < static_cast<idx_t>(bind_data.grid.count())) {
		result_data[count] = timestamp_t(bind_data.grid.at(static_cast<int64_t>(state.current_idx)));
		state.current_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// ts_rate — RANGE family aggregate, RAW_WINDOW state, EXTRAPOLATE edge mode,
// COUNTER domain: `counter_fold` + `extrapolate` composed exactly as
// `docs/design/primitives.md`'s own header states rate is
// (`docs/design/primitives.md:tier-ratio:` `with different plugs`). Two
// overloads share every callback below and differ only in whether a
// `start_ts` column is bound — `docs/design/architecture.md`'s own numeric
// contract (`docs/design/architecture.md:three-numeric-contracts:` `as an
// optional third input`):
//   ts_rate(ts, value, grid_start, grid_end, grid_step, window)
//   ts_rate(ts, value, start_ts, grid_start, grid_end, grid_step, window)
//===--------------------------------------------------------------------===//

// The per-group heap payload — what `state_alloc` places through DuckDB's
// tracked `ArenaAllocator` and `state_destroy` releases via its own
// destructor (`docs/testing/primitives.md:state_alloc-row:` `Allocate
// through DuckDB's tracked allocator`). `buffer` accumulates raw `(t, v)`
// samples across however many `Update` calls touch this group; `st_pairs`
// is the optional start-timestamp role, joined back onto `buffer`'s deduped
// samples by `t` at finalize time — the same join
// `test/kernel/rate_fixture_eval.hpp:EvaluateRate:` `doesn't travel through`
// performs for the L2 fixture loader, since `SampleBuffer` itself carries no
// `st` field.
struct RatePayload {
	SampleBuffer buffer;
	vector<std::pair<int64_t, int64_t>> st_pairs;
};

struct RateAggState {
	RatePayload *payload;
};

struct RateBindData : public FunctionData {
	RateBindData(bool has_st_p, Grid grid_p, int64_t window_p) : has_st(has_st_p), grid(grid_p), window(window_p) {
	}

	bool has_st;
	Grid grid;
	int64_t window;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<RateBindData>(has_st, grid, window);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<RateBindData>();
		return has_st == other.has_st && window == other.window && grid.start == other.grid.start &&
		       grid.end == other.grid.end && grid.step == other.grid.step;
	}
};

idx_t RateStateSize(const AggregateFunction &) {
	return sizeof(RateAggState);
}

void RateStateInit(const AggregateFunction &, data_ptr_t state_p) {
	reinterpret_cast<RateAggState *>(state_p)->payload = nullptr;
}

// bind_aggregate_function_t for both `ts_rate` overloads: the two
// signatures differ only in argument count (6 without `start_ts`, 7 with),
// so one bind branches on `arguments.size()` rather than duplicating the
// grid-argument validation twice.
unique_ptr<FunctionData> BindTsRate(ClientContext &context, AggregateFunction &function,
                                    vector<unique_ptr<Expression>> &arguments) {
	bool has_st = arguments.size() == 7;
	idx_t grid_start_idx = has_st ? 3 : 2;

	Value grid_start_v = RequireConstantArg(context, arguments, grid_start_idx, "grid_start");
	Value grid_end_v = RequireConstantArg(context, arguments, grid_start_idx + 1, "grid_end");
	Value grid_step_v = RequireConstantArg(context, arguments, grid_start_idx + 2, "grid_step");
	Value window_v = RequireConstantArg(context, arguments, grid_start_idx + 3, "window");

	int64_t window = window_v.GetValue<int64_t>();
	if (window <= 0) {
		throw BinderException("ts_rate: window must be positive, got %lld", (long long)window);
	}

	Grid grid = BindGridArgs(grid_start_v.GetValue<timestamp_t>().value, grid_end_v.GetValue<timestamp_t>().value,
	                         grid_step_v.GetValue<int64_t>());
	return make_uniq<RateBindData>(has_st, grid, window);
}

template <bool HAS_ST>
void RateUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, Vector &state_vector,
                idx_t count) {
	UnifiedVectorFormat ts_data, value_data, st_data, state_data;
	inputs[0].ToUnifiedFormat(count, ts_data);
	inputs[1].ToUnifiedFormat(count, value_data);
	if (HAS_ST) {
		inputs[2].ToUnifiedFormat(count, st_data);
	}
	state_vector.ToUnifiedFormat(count, state_data);

	auto ts_ptr = UnifiedVectorFormat::GetData<timestamp_t>(ts_data);
	auto value_ptr = UnifiedVectorFormat::GetData<double>(value_data);
	auto st_ptr = HAS_ST ? UnifiedVectorFormat::GetData<timestamp_t>(st_data) : nullptr;
	auto states = UnifiedVectorFormat::GetData<RateAggState *>(state_data);

	for (idx_t i = 0; i < count; i++) {
		auto ts_idx = ts_data.sel->get_index(i);
		auto value_idx = value_data.sel->get_index(i);
		if (!ts_data.validity.RowIsValid(ts_idx) || !value_data.validity.RowIsValid(value_idx)) {
			continue; // NULL ts or value: dropped before the fold, never counted as a sample
		}
		auto &state = *states[state_data.sel->get_index(i)];
		if (state.payload == nullptr) {
			state.payload = aggr_input_data.allocator.Make<RatePayload>();
		}
		int64_t t = ts_ptr[ts_idx].value;
		state.payload->buffer.append(t, value_ptr[value_idx]);
		if (HAS_ST) {
			auto st_idx = st_data.sel->get_index(i);
			if (st_data.validity.RowIsValid(st_idx)) {
				state.payload->st_pairs.emplace_back(t, st_ptr[st_idx].value);
			}
		}
	}
}

// Generic combine (Tier 6's own row): the non-destructive path merges
// `source`'s buffer into `target`'s via `SampleBuffer::merge`
// (`src/kernel/sample_buffer.hpp:SampleBuffer:` `void merge(const
// SampleBuffer &other)`), leaving `source` untouched; the destructive path
// DuckDB signals via `AggregateCombineType::ALLOW_DESTRUCTIVE` instead moves
// the whole heap payload and nulls `source`'s own pointer so `RateDestroy`
// never double-frees it.
void RateCombine(Vector &state_vector, Vector &combined, AggregateInputData &aggr_input_data, idx_t count) {
	UnifiedVectorFormat sdata;
	state_vector.ToUnifiedFormat(count, sdata);
	auto source_ptr = UnifiedVectorFormat::GetData<RateAggState *>(sdata);
	auto target_ptr = FlatVector::GetData<RateAggState *>(combined);
	bool destructive = aggr_input_data.combine_type == AggregateCombineType::ALLOW_DESTRUCTIVE;

	for (idx_t i = 0; i < count; i++) {
		auto &source = *source_ptr[sdata.sel->get_index(i)];
		auto &target = *target_ptr[i];
		if (source.payload == nullptr) {
			continue;
		}
		if (target.payload == nullptr) {
			if (destructive) {
				target.payload = source.payload;
				source.payload = nullptr;
				continue;
			}
			target.payload = aggr_input_data.allocator.Make<RatePayload>();
		}
		target.payload->buffer.merge(source.payload->buffer);
		target.payload->st_pairs.insert(target.payload->st_pairs.end(), source.payload->st_pairs.begin(),
		                                source.payload->st_pairs.end());
	}
}

// state_destroy (Tier 6's own row): invoked once per state — DuckDB tears
// down every live aggregate state on any exit path from the query,
// including cancellation and an exception raised elsewhere in the same
// query — and releases `RatePayload`'s real heap resources (`SampleBuffer`'s
// own arena pages, `st_pairs`' heap buffer) via its destructor. The
// payload's own footprint came from the tracked `ArenaAllocator`
// (`RateUpdate`'s `Make<RatePayload>` call) and is reclaimed with that
// allocator's own lifetime, never freed individually here.
void RateDestroy(Vector &state_vector, AggregateInputData &aggr_input_data, idx_t count) {
	UnifiedVectorFormat sdata;
	state_vector.ToUnifiedFormat(count, sdata);
	auto states = UnifiedVectorFormat::GetData<RateAggState *>(sdata);
	for (idx_t i = 0; i < count; i++) {
		auto &state = *states[sdata.sel->get_index(i)];
		if (state.payload != nullptr) {
			state.payload->~RatePayload();
			state.payload = nullptr;
		}
	}
}

struct RatePoint {
	bool has_value = false;
	double value = 0.0;
};

// The composition primitives.md's own header names — `window_walk` (Tier 3)
// then `counter_fold` + `extrapolate` (Tier 4) at every grid point, then
// `rate = extrapolate(...).value / window` — the same pipeline
// `test/kernel/rate_fixture_eval.hpp:EvaluateRate:` `Divide by the nominal
// window width` already proves against the L2 fixture roster; this issue's
// own host glue wires it into a real aggregate state instead of a fresh
// buffer built from a fixture file.
vector<RatePoint> ComputeRatePoints(RatePayload &payload, const Grid &grid, int64_t window, bool has_st) {
	payload.buffer.sort_dedup();

	std::map<int64_t, int64_t> st_by_t;
	if (has_st) {
		for (auto &pair : payload.st_pairs) {
			st_by_t[pair.first] = pair.second;
		}
	}

	SampleSpan all = payload.buffer.slice(std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());

	vector<CounterSample> data;
	data.reserve(all.size());
	for (const Sample *it = all.begin; it != all.end; ++it) {
		CounterSample cs;
		cs.t = it->t;
		cs.v = it->v;
		auto found = st_by_t.find(it->t);
		if (found != st_by_t.end()) {
			cs.has_st = true;
			cs.st = found->second;
		}
		data.push_back(cs);
	}

	vector<WindowRange> ranges = chronoduck::window_walk(all.begin, all.size(), grid, window);

	vector<RatePoint> out;
	out.reserve(ranges.size());
	for (idx_t gi = 0; gi < ranges.size(); gi++) {
		const WindowRange &r = ranges[gi];
		int64_t anchor = grid.at(static_cast<int64_t>(gi));
		int64_t window_start = anchor - window;

		const CounterSample *sub = (r.hi > r.lo) ? &data[r.lo] : nullptr;
		std::size_t n = r.hi - r.lo;
		CounterFoldSummary summary =
		    chronoduck::counter_fold(sub, n, nullptr, [](const CounterSample &p, const CounterSample &c) {
			    return chronoduck::value_or_st_reset(p, c, /*from_delta_temporality=*/false);
		    });
		ExtrapolateResult ex = chronoduck::extrapolate(summary, window_start, anchor, /*is_counter=*/true);

		RatePoint point;
		point.has_value = ex.has_value;
		if (ex.has_value) {
			point.value = ex.value / static_cast<double>(window);
		}
		out.push_back(point);
	}
	return out;
}

// finalize_to_list (Tier 6's own row): one `LIST(DOUBLE)` per group, grid
// order, NULL entries where `extrapolate` found no value
// (`docs/testing/primitives.md:finalize_to_list-row:` `offsets are
// contiguous and sum to child size`). A group with no rows at all (every
// input filtered away) gets a NULL list rather than a list of all-NULL
// entries — the same posture DuckDB's own `histogram`/`list` aggregates
// take for an empty state.
void RateFinalize(Vector &state_vector, AggregateInputData &aggr_input_data, Vector &result, idx_t count,
                  idx_t offset) {
	auto &bind_data = aggr_input_data.bind_data->Cast<RateBindData>();

	UnifiedVectorFormat sdata;
	state_vector.ToUnifiedFormat(count, sdata);
	auto states = UnifiedVectorFormat::GetData<RateAggState *>(sdata);

	auto &mask = FlatVector::Validity(result);
	auto result_data = FlatVector::GetData<list_entry_t>(result);
	idx_t old_len = ListVector::GetListSize(result);

	vector<vector<RatePoint>> per_group(count);
	idx_t new_entries = 0;
	for (idx_t i = 0; i < count; i++) {
		auto &state = *states[sdata.sel->get_index(i)];
		if (state.payload != nullptr) {
			per_group[i] = ComputeRatePoints(*state.payload, bind_data.grid, bind_data.window, bind_data.has_st);
			new_entries += per_group[i].size();
		}
	}

	ListVector::Reserve(result, old_len + new_entries);
	auto &child = ListVector::GetEntry(result);
	auto &child_validity = FlatVector::Validity(child);
	auto child_data = FlatVector::GetData<double>(child);

	idx_t current_offset = old_len;
	for (idx_t i = 0; i < count; i++) {
		const auto rid = i + offset;
		auto &state = *states[sdata.sel->get_index(i)];
		result_data[rid].offset = current_offset;
		if (state.payload == nullptr) {
			mask.SetInvalid(rid);
			result_data[rid].length = 0;
			continue;
		}
		for (const RatePoint &point : per_group[i]) {
			if (point.has_value) {
				child_data[current_offset] = point.value;
			} else {
				child_validity.SetInvalid(current_offset);
			}
			current_offset++;
		}
		result_data[rid].length = current_offset - result_data[rid].offset;
	}
	ListVector::SetListSize(result, current_offset);
	result.Verify(count);
}

AggregateFunction MakeTsRateAggregate(bool has_st) {
	vector<LogicalType> arguments = {LogicalType::TIMESTAMP, LogicalType::DOUBLE};
	if (has_st) {
		arguments.push_back(LogicalType::TIMESTAMP); // start_ts
	}
	arguments.push_back(LogicalType::TIMESTAMP); // grid_start
	arguments.push_back(LogicalType::TIMESTAMP); // grid_end
	arguments.push_back(LogicalType::BIGINT);    // grid_step, ticks (microseconds)
	arguments.push_back(LogicalType::BIGINT);    // window, ticks (microseconds)

	return AggregateFunction("ts_rate", arguments, LogicalType::LIST(LogicalType::DOUBLE), RateStateSize, RateStateInit,
	                         has_st ? RateUpdate<true> : RateUpdate<false>, RateCombine, RateFinalize,
	                         FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                         /*simple_update=*/nullptr, BindTsRate, RateDestroy);
}

// Registration functions, one per registry.def row, named Register_<name> so
// LoadInternal's registry.def-driven dispatch below can call each by
// token-pasting its row name onto "Register_" — no per-row special-casing.
// Bodies are unchanged from before this PR; only the wrapping function is new.

void Register_chronoduck_version(ExtensionLoader &loader) {
	auto chronoduck_version_scalar_function =
	    ScalarFunction("chronoduck_version", {}, LogicalType::VARCHAR, ChronoduckVersionScalarFun);
	loader.RegisterFunction(chronoduck_version_scalar_function);
}

void Register_ts_grid_index(ExtensionLoader &loader) {
	auto ts_grid_index_scalar_function =
	    ScalarFunction("ts_grid_index", {LogicalType::TIMESTAMP, LogicalType::TIMESTAMP, LogicalType::BIGINT},
	                   LogicalType::BIGINT, TsGridIndexScalarFun);
	loader.RegisterFunction(ts_grid_index_scalar_function);
}

void Register_ts_grid(ExtensionLoader &loader) {
	TableFunction ts_grid_function("ts_grid", {LogicalType::TIMESTAMP, LogicalType::TIMESTAMP, LogicalType::BIGINT},
	                               TsGridFunction, TsGridBind, TsGridInit);
	loader.RegisterFunction(ts_grid_function);
}

void Register_ts_rate(ExtensionLoader &loader) {
	AggregateFunctionSet ts_rate_function_set("ts_rate");
	ts_rate_function_set.AddFunction(MakeTsRateAggregate(/*has_st=*/false));
	ts_rate_function_set.AddFunction(MakeTsRateAggregate(/*has_st=*/true));
	loader.RegisterFunction(ts_rate_function_set);
}

} // namespace

// registry.def-driven dispatch (Article V.1): every row in
// src/kernel/registry.def is registered here by token-pasting "Register_"
// onto the row's name — chronoduck_version -> Register_chronoduck_version,
// ts_grid_index -> Register_ts_grid_index, above. A row added to
// registry.def without a matching Register_<name> function fails this
// translation unit's build, named after the row (the macro-pasted call site
// simply won't resolve).
static void LoadInternal(ExtensionLoader &loader) {
#define TS_FN(name, family, state, det, edge, domain, scale) Register_##name(loader);
#include "kernel/registry.def"
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
