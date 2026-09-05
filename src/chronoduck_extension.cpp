#define DUCKDB_EXTENSION_MAIN

#include "chronoduck_extension.hpp"
#include "kernel/counter_fold.hpp"
#include "kernel/extrapolate.hpp"
#include "kernel/grid.hpp"
#include "kernel/grid_stream.hpp"
#include "kernel/registry_types.hpp"
#include "kernel/sample_buffer.hpp"
#include "kernel/scan_bounds.hpp"
#include "kernel/window_walk.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/sorting/sort_strategy.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/filter/physical_filter.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
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

//===--------------------------------------------------------------------===//
// ts_rate, the operator form — a custom sink-and-source physical operator
// (`docs/decisions/0003-operator-as-partition-sort-sink.md`,
// `docs/decisions/0018-partition-sort-sink-over-windowed-aggregates.md`):
// partition by series hash, spillable per-partition sort by `ts` (DuckDB's
// own `SortStrategy` — the same generic partition-and-sort machinery
// `PhysicalWindow` and `PhysicalAsOfJoin` are built on, not a hand-rolled
// thread pool or a no-spill buffer, the two spike simplifications ADR 0018
// names as *not* to inherit unmeasured), then `grid_stream`
// (`src/kernel/grid_stream.hpp`) walks each partition's sorted stream holding
// one window resident, released at the series boundary.
//
// Registered as a second, table-function overload of the *same* SQL name
// `ts_rate` (DuckDB's own `range` is scalar-context and table-context at
// once; a table function and an aggregate are different catalog entries, so
// this adds no new row to `src/kernel/registry.def` — Article V.1's fence is
// the *name* `ts_rate`, which already has one). Reached through a table
// function's `bind_operator` hook (`table_function_bind_operator_t`), the
// one DuckDB entry point that lets an out-of-tree extension hand back an
// arbitrary `LogicalOperator` — here a `LogicalExtensionOperator` whose
// `CreatePlan` builds the physical sink-and-source directly, no
// `OptimizerExtension` needed for this single-child shape (that heavier
// mechanism is what the *anchored* fold form, out of this issue's scope,
// would need for a second, relation-valued child — ADR 0018's own closing
// paragraph).
//
// SQL shape: `ts_rate(TABLE (SELECT series_id, ts, value[, start_ts] FROM
// ...), grid_start, grid_end, grid_step, window)` — one row per series,
// `(series_id, values LIST(DOUBLE))`, grid order, directly comparable to the
// aggregate form's own `GROUP BY series_id` output.
//===--------------------------------------------------------------------===//

// The operator's own bind data: which column of the input relation plays
// which numeric-contract role (resolved by name, not position, since the
// input arrives as a single `TABLE` argument rather than positional scalar
// columns), plus the same grid/window bind data `RateBindData` carries for
// the aggregate form.
struct RangeStreamBindData {
	bool has_st = false;
	Grid grid {0, 0, 1};
	int64_t window = 1;
	idx_t series_idx = 0;
	idx_t ts_idx = 0;
	idx_t value_idx = 0;
	idx_t start_ts_idx = 0; // meaningful only when has_st
	LogicalType series_type = LogicalType::UBIGINT;
};

class PhysicalGridStream : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	PhysicalGridStream(PhysicalPlan &physical_plan, PhysicalOperator &child, RangeStreamBindData bind_data_p,
	                   vector<LogicalType> types_p, idx_t estimated_cardinality)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types_p), estimated_cardinality),
	      bind_data(std::move(bind_data_p)) {
		children.push_back(child);
	}

	RangeStreamBindData bind_data;

public:
	// Sink interface — pure delegation to `SortStrategy`, the same shape
	// `PhysicalWindow::Sink`/`Combine`/`Finalize` use: this operator never
	// touches raw chunks itself, only the partitioned-and-sorted result.
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;

	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return true;
	}

public:
	// Source interface — one thread per hash bin, claimed from a shared
	// atomic counter. A bin is a coarse hash partition, not one series each:
	// `SortStrategy` sorts a bin by `(series_id, ts)` together (partition
	// columns lead the sort key), so distinct series sharing a bin still
	// each get their own contiguous run, detected as a boundary where
	// `series_id` changes between consecutive rows — but two *different*
	// series can and do land in the same bin whenever the radix bit count
	// (chosen internally from estimated cardinality) is smaller than the
	// series count. Each claimed bin is streamed through `grid_stream` one
	// run at a time, holding one window resident per run, releasing it at
	// the series boundary before starting the next. N concurrently-active
	// threads therefore hold O(N × window) resident, never O(series ×
	// window) — the memory law this issue's second acceptance criterion
	// states — independent of how many series exist in total or how they
	// happen to fall into bins.
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context,
	                                                 GlobalSourceState &gstate) const override;

	bool IsSource() const override {
		return true;
	}
	bool ParallelSource() const override {
		return true;
	}

	// Output rows are one per series, in whichever order threads finish
	// claiming hash bins — unrelated to the input's own row order, the same
	// posture `PhysicalWindow::SourceOrder` takes once any partitioning is in
	// play.
	OrderPreservationType SourceOrder() const override {
		return OrderPreservationType::NO_ORDER;
	}

	InsertionOrderPreservingMap<string> ParamsToString() const override {
		InsertionOrderPreservingMap<string> result;
		result["Function"] = "ts_rate (operator)";
		SetEstimatedCardinality(result, estimated_cardinality);
		return result;
	}

protected:
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

public:
	class GlobalSourceStateImpl;
	class LocalSourceStateImpl;

private:
	// Every distinct-series run found in `bin`, each as (series_id value,
	// per-grid-point results) — usually one entry, more whenever the bin
	// collides two or more series together (see the Source interface
	// comment above).
	vector<std::pair<Value, vector<RatePoint>>> ProcessOneBin(ExecutionContext &context, idx_t bin,
	                                                          GlobalSourceStateImpl &gsource,
	                                                          OperatorSourceInput &input) const;
	static void WriteOutputRow(DataChunk &chunk, idx_t out_row, const Value &series_id_value,
	                           const vector<RatePoint> &points);
};

class GridStreamGlobalSinkState : public GlobalSinkState {
public:
	GridStreamGlobalSinkState(ClientContext &client, const PhysicalGridStream &op) {
		vector<unique_ptr<Expression>> partition_bys;
		partition_bys.push_back(make_uniq<BoundReferenceExpression>(op.bind_data.series_type, op.bind_data.series_idx));
		vector<BoundOrderByNode> order_bys;
		order_bys.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST,
		                       make_uniq<BoundReferenceExpression>(LogicalType::TIMESTAMP, op.bind_data.ts_idx));
		const vector<unique_ptr<BaseStatistics>> partition_stats;
		sort_strategy = SortStrategy::Factory(client, partition_bys, order_bys, op.children[0].get().GetTypes(),
		                                      partition_stats, op.estimated_cardinality, /*require_payload=*/false);
		strategy_sink = sort_strategy->GetGlobalSinkState(client);
	}

	unique_ptr<SortStrategy> sort_strategy;
	unique_ptr<GlobalSinkState> strategy_sink;
};

class GridStreamLocalSinkState : public LocalSinkState {
public:
	GridStreamLocalSinkState(ExecutionContext &context, GridStreamGlobalSinkState &gstate)
	    : local_group(gstate.sort_strategy->GetLocalSinkState(context)) {
	}
	unique_ptr<LocalSinkState> local_group;
};

unique_ptr<GlobalSinkState> PhysicalGridStream::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<GridStreamGlobalSinkState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalGridStream::GetLocalSinkState(ExecutionContext &context) const {
	auto &gstate = sink_state->Cast<GridStreamGlobalSinkState>();
	return make_uniq<GridStreamLocalSinkState>(context, gstate);
}

SinkResultType PhysicalGridStream::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &sink) const {
	auto &gstate = sink.global_state.Cast<GridStreamGlobalSinkState>();
	auto &lstate = sink.local_state.Cast<GridStreamLocalSinkState>();
	OperatorSinkInput hsink {*gstate.strategy_sink, *lstate.local_group, sink.interrupt_state};
	return gstate.sort_strategy->Sink(context, chunk, hsink);
}

SinkCombineResultType PhysicalGridStream::Combine(ExecutionContext &context, OperatorSinkCombineInput &combine) const {
	auto &gstate = combine.global_state.Cast<GridStreamGlobalSinkState>();
	auto &lstate = combine.local_state.Cast<GridStreamLocalSinkState>();
	OperatorSinkCombineInput hcombine {*gstate.strategy_sink, *lstate.local_group, combine.interrupt_state};
	return gstate.sort_strategy->Combine(context, hcombine);
}

SinkFinalizeType PhysicalGridStream::Finalize(Pipeline &pipeline, Event &event, ClientContext &client,
                                              OperatorSinkFinalizeInput &input) const {
	auto &gsink = input.global_state.Cast<GridStreamGlobalSinkState>();
	OperatorSinkFinalizeInput hfinalize {*gsink.strategy_sink, input.interrupt_state};
	return gsink.sort_strategy->Finalize(client, hfinalize);
}

// The per-grid-point result `grid_stream`'s `emit` callback fills in,
// finalized into one `LIST(DOUBLE)` row per series — reuses `RatePoint`, the
// same struct `ComputeRatePoints`/`RateFinalize` already use for the
// aggregate form's identical output shape.

class PhysicalGridStream::GlobalSourceStateImpl : public GlobalSourceState {
public:
	GlobalSourceStateImpl(ClientContext &client, GridStreamGlobalSinkState &gsink_p) : gsink(gsink_p) {
		hashed_source = gsink.sort_strategy->GetGlobalSourceState(client, *gsink.strategy_sink);
		auto &groups = gsink.sort_strategy->GetHashGroups(*hashed_source);
		for (idx_t i = 0; i < groups.size(); i++) {
			if (groups[i].chunks > 0) {
				non_empty_bins.push_back(i);
			}
		}
	}

	idx_t MaxThreads() override {
		return std::max<idx_t>(1, non_empty_bins.size());
	}

	GridStreamGlobalSinkState &gsink;
	unique_ptr<GlobalSourceState> hashed_source;
	vector<idx_t> non_empty_bins;
	atomic<idx_t> next_bin {0};
};

unique_ptr<GlobalSourceState> PhysicalGridStream::GetGlobalSourceState(ClientContext &context) const {
	auto &gsink = sink_state->Cast<GridStreamGlobalSinkState>();
	return make_uniq<GlobalSourceStateImpl>(context, gsink);
}

// Ready-to-emit output rows this thread has already computed, queued for
// writing into a DuckDB-supplied output chunk: `ProcessOneBin` fully drains
// one hash bin (which may hold several distinct series, see the Source
// interface comment above) into this queue in one call, and `GetDataInternal`
// pulls from it across as many calls as it takes to drain — never more than
// one bin's worth of *output* queued at a time per thread.
class PhysicalGridStream::LocalSourceStateImpl : public LocalSourceState {
public:
	vector<std::pair<Value, vector<RatePoint>>> pending_rows;
	idx_t pending_idx = 0;
};

unique_ptr<LocalSourceState> PhysicalGridStream::GetLocalSourceState(ExecutionContext &context,
                                                                     GlobalSourceState &gstate) const {
	return make_uniq<LocalSourceStateImpl>();
}

// Sorts, materializes and streams exactly one hash bin end to end on the
// calling thread, releasing one series' window state before starting the
// next whenever `series_id` changes between consecutive rows (bins are
// sorted by `(series_id, ts)` together, so every series' own rows are
// contiguous within the bin even when several series share it). Never
// called twice for the same `bin` (each is claimed exactly once from
// `gsource.next_bin`), so this never races another thread over the same
// partition's state.
vector<std::pair<Value, vector<RatePoint>>> PhysicalGridStream::ProcessOneBin(ExecutionContext &context, idx_t bin,
                                                                              GlobalSourceStateImpl &gsource,
                                                                              OperatorSourceInput &input) const {
	auto &sort_strategy = *gsource.gsink.sort_strategy;

	auto local_unused = make_uniq<LocalSourceState>();
	OperatorSourceInput hashed_input {*gsource.hashed_source, *local_unused, input.interrupt_state};

	{
		OperatorSinkFinalizeInput finalize {*gsource.gsink.strategy_sink, input.interrupt_state};
		sort_strategy.SortColumnData(context, bin, finalize);
	}
	while (sort_strategy.MaterializeColumnData(context, bin, hashed_input) == SourceResultType::HAVE_MORE_OUTPUT) {
		// Loop until this bin's partition is fully materialized — this
		// thread owns `bin` exclusively, so no other caller can finish it out
		// from under it.
	}
	auto coll = sort_strategy.GetColumnData(bin, hashed_input);
	if (!coll) {
		throw InternalException("ts_rate (operator): SortStrategy did not return the materialized partition for a "
		                        "bin this thread exclusively owns");
	}

	vector<std::pair<Value, vector<RatePoint>>> rows;
	if (coll->Count() == 0) {
		return rows;
	}

	// The currently-open series run: `active_points`/`active_stream` are
	// reset every time `series_id` changes, so at most one series' window
	// state is ever resident at a time within this bin.
	unique_ptr<chronoduck::GridStream> active_stream;
	unique_ptr<vector<RatePoint>> active_points;
	Value active_series_id;
	bool have_active = false;

	auto emit = [&](int64_t grid_index, const CounterSample *data, std::size_t n) {
		const CounterSample *sub = n > 0 ? data : nullptr;
		CounterFoldSummary summary =
		    chronoduck::counter_fold(sub, n, nullptr, [](const CounterSample &p, const CounterSample &c) {
			    return chronoduck::value_or_st_reset(p, c, /*from_delta_temporality=*/false);
		    });
		int64_t anchor = bind_data.grid.at(grid_index);
		int64_t window_start = anchor - bind_data.window;
		ExtrapolateResult ex = chronoduck::extrapolate(summary, window_start, anchor, /*is_counter=*/true);
		RatePoint &point = (*active_points)[static_cast<std::size_t>(grid_index)];
		point.has_value = ex.has_value;
		if (ex.has_value) {
			point.value = ex.value / static_cast<double>(bind_data.window);
		}
	};

	auto flush_active = [&]() {
		if (!have_active) {
			return;
		}
		active_stream->end();
		active_stream->resume(emit);
		rows.emplace_back(std::move(active_series_id), std::move(*active_points));
		have_active = false;
	};
	auto start_series = [&](Value series_id_value) {
		flush_active();
		active_series_id = std::move(series_id_value);
		active_points = make_uniq<vector<RatePoint>>(static_cast<std::size_t>(bind_data.grid.count()));
		active_stream = make_uniq<chronoduck::GridStream>(bind_data.grid, bind_data.window);
		have_active = true;
	};

	ColumnDataScanState scan_state;
	coll->InitializeScan(scan_state);
	DataChunk in_chunk;
	coll->InitializeScanChunk(scan_state, in_chunk);
	while (coll->Scan(scan_state, in_chunk)) {
		idx_t count = in_chunk.size();

		UnifiedVectorFormat ts_data, value_data, st_data;
		in_chunk.data[bind_data.ts_idx].ToUnifiedFormat(count, ts_data);
		in_chunk.data[bind_data.value_idx].ToUnifiedFormat(count, value_data);
		if (bind_data.has_st) {
			in_chunk.data[bind_data.start_ts_idx].ToUnifiedFormat(count, st_data);
		}
		auto ts_ptr = UnifiedVectorFormat::GetData<timestamp_t>(ts_data);
		auto value_ptr = UnifiedVectorFormat::GetData<double>(value_data);
		auto st_ptr = bind_data.has_st ? UnifiedVectorFormat::GetData<timestamp_t>(st_data) : nullptr;

		for (idx_t i = 0; i < count; i++) {
			Value row_series_id = in_chunk.GetValue(bind_data.series_idx, i);
			if (!have_active || row_series_id != active_series_id) {
				start_series(std::move(row_series_id));
			}

			auto ts_idx_u = ts_data.sel->get_index(i);
			auto value_idx_u = value_data.sel->get_index(i);
			// The upstream `PhysicalFilter` (`LogicalGridStream::CreatePlan`)
			// already excludes NULL ts/value rows before the sink, so every
			// row reaching this scan is valid — no re-check needed here.
			CounterSample cs;
			cs.t = ts_ptr[ts_idx_u].value;
			cs.v = value_ptr[value_idx_u];
			if (bind_data.has_st) {
				auto st_idx_u = st_data.sel->get_index(i);
				if (st_data.validity.RowIsValid(st_idx_u)) {
					cs.has_st = true;
					cs.st = st_ptr[st_idx_u].value;
				}
			}
			active_stream->feed(cs, emit);
		}
	}
	flush_active();

	return rows;
}

void PhysicalGridStream::WriteOutputRow(DataChunk &chunk, idx_t out_row, const Value &series_id_value,
                                        const vector<RatePoint> &points) {
	chunk.SetValue(0, out_row, series_id_value);

	auto &list_vec = chunk.data[1];
	auto result_data = FlatVector::GetData<list_entry_t>(list_vec);
	idx_t old_len = ListVector::GetListSize(list_vec);
	ListVector::Reserve(list_vec, old_len + points.size());
	auto &child = ListVector::GetEntry(list_vec);
	auto &child_validity = FlatVector::Validity(child);
	auto child_data = FlatVector::GetData<double>(child);
	idx_t offset = old_len;
	for (const RatePoint &point : points) {
		if (point.has_value) {
			child_data[offset] = point.value;
		} else {
			child_validity.SetInvalid(offset);
		}
		offset++;
	}
	result_data[out_row] = {old_len, points.size()};
	ListVector::SetListSize(list_vec, offset);
}

SourceResultType PhysicalGridStream::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                     OperatorSourceInput &input) const {
	auto &gsource = input.global_state.Cast<GlobalSourceStateImpl>();
	auto &lstate = input.local_state.Cast<LocalSourceStateImpl>();
	idx_t produced = 0;
	while (produced < STANDARD_VECTOR_SIZE) {
		if (lstate.pending_idx >= lstate.pending_rows.size()) {
			idx_t slot = gsource.next_bin.fetch_add(1);
			if (slot >= gsource.non_empty_bins.size()) {
				break;
			}
			lstate.pending_rows = ProcessOneBin(context, gsource.non_empty_bins[slot], gsource, input);
			lstate.pending_idx = 0;
			if (lstate.pending_rows.empty()) {
				continue; // an empty bin (shouldn't happen for a non_empty_bins entry, but harmless if it does)
			}
		}
		auto &row = lstate.pending_rows[lstate.pending_idx];
		WriteOutputRow(chunk, produced, row.first, row.second);
		lstate.pending_idx++;
		produced++;
	}
	chunk.SetCardinality(produced);
	return produced > 0 ? SourceResultType::HAVE_MORE_OUTPUT : SourceResultType::FINISHED;
}

// The logical-plan side of the `bind_operator` hook: built directly by
// `GridStreamBindOperator` below, with no `OptimizerExtension` in between —
// robust to `enable_optimizer=false` (DuckDB's own "unoptimized" statement
// verifier, run automatically alongside every query) precisely because
// nothing about wiring this node's child depends on the optimizer running at
// all. `bind_table_function.cpp`'s own binder wires the `TABLE` argument's
// bound subquery in beneath *whichever leaf* the returned plan bottoms out
// at, asserting that leaf is a `LogicalGet` — so `GridStreamBindOperator`
// gives this node a one-column-passthrough `LogicalGet` child of its own
// (see `GridStreamPassthroughFn`) for the subquery to land under, rather
// than being that leaf itself.
class LogicalGridStream : public LogicalExtensionOperator {
public:
	LogicalGridStream(idx_t bind_index_p, RangeStreamBindData bind_data_p)
	    : bind_index(bind_index_p), stream_bind_data(std::move(bind_data_p)) {
		SetEstimatedCardinality(1000);
	}

	idx_t bind_index;
	RangeStreamBindData stream_bind_data;

	void ResolveTypes() override {
		types = {stream_bind_data.series_type, LogicalType::LIST(LogicalType::DOUBLE)};
	}

	vector<ColumnBinding> GetColumnBindings() override {
		return GenerateColumnBindings(bind_index, types.size());
	}

	string GetName() const override {
		return "GRID_STREAM";
	}

	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override {
		auto &child = planner.CreatePlan(*children[0]);

		// Drop NULL ts/value rows before they ever reach the sink, the same
		// posture `RateUpdate` applies per row for the aggregate form (see
		// its own "NULL ts or value" comment).
		vector<unique_ptr<Expression>> filters;
		auto ts_not_null =
		    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
		ts_not_null->children.push_back(
		    make_uniq<BoundReferenceExpression>(LogicalType::TIMESTAMP, stream_bind_data.ts_idx));
		filters.push_back(std::move(ts_not_null));
		auto value_not_null =
		    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
		value_not_null->children.push_back(
		    make_uniq<BoundReferenceExpression>(LogicalType::DOUBLE, stream_bind_data.value_idx));
		filters.push_back(std::move(value_not_null));

		auto &filtered = planner.Make<PhysicalFilter>(child.GetTypes(), std::move(filters), estimated_cardinality);
		filtered.children.push_back(child);

		return planner.Make<PhysicalGridStream>(filtered, stream_bind_data, types, estimated_cardinality);
	}
};

// GridStreamPassthroughFn — the `in_out_function` body for the one-column
// passthrough `LogicalGet` `GridStreamBindOperator` gives `LogicalGridStream`
// as its child: references every input column into the output unchanged.
// Its only job is to be a real `LogicalGet` for the generic subquery-attach
// code to descend to (see `LogicalGridStream`'s own header comment) — it
// never does anything to the data itself.
OperatorResultType GridStreamPassthroughFn(ExecutionContext &context, TableFunctionInput &data, DataChunk &input,
                                           DataChunk &output) {
	output.SetCardinality(input.size());
	for (idx_t col = 0; col < input.ColumnCount(); col++) {
		output.data[col].Reference(input.data[col]);
	}
	return OperatorResultType::NEED_MORE_INPUT;
}

// table_function_bind_operator_t for `ts_rate`'s operator overload: resolves
// the input relation's column roles *by name* (`series_id`, `ts`, `value`,
// optional `start_ts`) — the `TABLE` argument carries no positional
// convention of its own — reusing `BindGridArgs`, the same grid/window
// bind-time validation `BindTsRate` applies for the aggregate form.
unique_ptr<LogicalOperator> GridStreamBindOperator(ClientContext &context, TableFunctionBindInput &input,
                                                   idx_t bind_index, vector<string> &return_names) {
	auto &names = input.input_table_names;
	auto &col_types = input.input_table_types;

	auto find_col = [&](const char *role) -> idx_t {
		for (idx_t i = 0; i < names.size(); i++) {
			if (names[i] == role) {
				return i;
			}
		}
		return DConstants::INVALID_INDEX;
	};

	idx_t series_idx = find_col("series_id");
	idx_t ts_idx = find_col("ts");
	idx_t value_idx = find_col("value");
	idx_t start_ts_idx = find_col("start_ts");
	if (series_idx == DConstants::INVALID_INDEX || ts_idx == DConstants::INVALID_INDEX ||
	    value_idx == DConstants::INVALID_INDEX) {
		throw BinderException("ts_rate: the TABLE argument must project columns named 'series_id', 'ts' and 'value'");
	}
	if (col_types[ts_idx] != LogicalType::TIMESTAMP) {
		throw BinderException("ts_rate: 'ts' column must be TIMESTAMP");
	}
	if (col_types[value_idx] != LogicalType::DOUBLE) {
		throw BinderException("ts_rate: 'value' column must be DOUBLE");
	}
	bool has_st = start_ts_idx != DConstants::INVALID_INDEX;
	if (has_st && col_types[start_ts_idx] != LogicalType::TIMESTAMP) {
		throw BinderException("ts_rate: 'start_ts' column must be TIMESTAMP");
	}

	if (input.inputs.size() != 5) {
		throw BinderException("ts_rate: expected (TABLE relation, grid_start, grid_end, grid_step, window)");
	}
	auto require = [&](idx_t idx, const char *name) -> Value {
		Value v = input.inputs[idx];
		if (v.IsNull()) {
			throw BinderException("ts_rate: %s must not be NULL", name);
		}
		return v;
	};
	Value grid_start_v = require(1, "grid_start");
	Value grid_end_v = require(2, "grid_end");
	Value grid_step_v = require(3, "grid_step");
	Value window_v = require(4, "window");

	int64_t window = window_v.GetValue<int64_t>();
	if (window <= 0) {
		throw BinderException("ts_rate: window must be positive, got %lld", (long long)window);
	}
	Grid grid = BindGridArgs(grid_start_v.GetValue<timestamp_t>().value, grid_end_v.GetValue<timestamp_t>().value,
	                         grid_step_v.GetValue<int64_t>());

	RangeStreamBindData bind_data;
	bind_data.has_st = has_st;
	bind_data.grid = grid;
	bind_data.window = window;
	bind_data.series_idx = series_idx;
	bind_data.ts_idx = ts_idx;
	bind_data.value_idx = value_idx;
	bind_data.start_ts_idx = has_st ? start_ts_idx : 0;
	bind_data.series_type = col_types[series_idx];

	if (!input.binder) {
		throw InternalException("ts_rate: bind_operator invoked without a binder");
	}
	TableFunction passthrough_fn("__ts_rate_operator_input", col_types, nullptr);
	passthrough_fn.in_out_function = GridStreamPassthroughFn;
	auto passthrough_get = make_uniq<LogicalGet>(input.binder->GenerateTableIndex(), passthrough_fn,
	                                             make_uniq<TableFunctionData>(), col_types, names);
	passthrough_get->input_table_types = col_types;
	passthrough_get->input_table_names = names;
	for (idx_t i = 0; i < col_types.size(); i++) {
		passthrough_get->AddColumnId(i);
	}

	auto logical_grid_stream = make_uniq<LogicalGridStream>(bind_index, std::move(bind_data));
	logical_grid_stream->children.push_back(std::move(passthrough_get));

	return_names = {"series_id", "values"};
	return std::move(logical_grid_stream);
}

TableFunction MakeTsRateOperatorTableFunction() {
	TableFunction fn(
	    "ts_rate",
	    {LogicalType::TABLE, LogicalType::TIMESTAMP, LogicalType::TIMESTAMP, LogicalType::BIGINT, LogicalType::BIGINT},
	    nullptr);
	fn.bind_operator = GridStreamBindOperator;
	return fn;
}

//===--------------------------------------------------------------------===//
// Scan-bound pushdown (T2.3) — an `OptimizerExtension` that pushes
// `scan_bounds`'s own `[start - window, end]` (`src/kernel/scan_bounds.hpp`'s
// own header comment: `ts_rate` claims only `EXTRAPOLATE`
// (`src/kernel/registry.def`'s own `ts_rate` row), which carries none of
// `scan_bounds`'s LOOKBACK/ANCHOR/SMOOTH extra terms, so `lookback` is always
// `0` here) as an ordinary `TableFilter` onto the `LogicalGet` beneath either
// plan shape `docs/design/architecture.md`'s own line names:
// `{Aggregate | ExtensionOperator} -> [Filter] -> Get`.
// (`docs/design/architecture.md:where-it-plugs-in:` `{Aggregate |
// ExtensionOperator} → [Filter] → Get`).
//
// Registered with `optimize_function`, not `pre_optimize_function` — the
// vendored `duckdb` submodule's own `optimizer_extension.hpp` documents
// `optimize_function` as running "after the DuckDB optimizers have run" (not
// cited in `file:construct:` form: the `hygiene` CI job never checks out
// that submodule, so a citation into it would fail there even though it's
// true). DuckDB's own filter-pushdown pass has therefore already collapsed a
// fully pushed-down `Filter -> Get` into a bare `Get` by the time this runs,
// which is exactly the `[Filter]` bracket's own optionality.
//===--------------------------------------------------------------------===//

// The extension setting the pushdown rule's own sentinel test flips off
// (`docs/testing/primitives.md:bind-scan_bounds-rows-scanned-row:` `a
// sentinel asserts that removing the bound multiplies rows scanned by ≥
// 10×`) — the only way to observe the rule's absence without a second build.
// Defaults on: production queries always get the pushdown.
const char *const kScanBoundPushdownSetting = "chronoduck_scan_bound_pushdown";

// The synthetic passthrough `LogicalGet`'s own table function name
// (`GridStreamBindOperator` above: `TableFunction passthrough_fn
// ("__ts_rate_operator_input", col_types, nullptr);`) — not a real scan, so
// both traversals below see through it rather than mistaking it for the
// pattern's own terminal `Get`.
const char *const kOperatorPassthroughFunctionName = "__ts_rate_operator_input";

// The only scan function this rule ever pushes a filter onto: DuckDB's own
// native table scan. `docs/design/architecture.md`'s own line also names
// Parquet scans in scope (`docs/design/architecture.md:where-it-plugs-in:`
// `on native and Parquet scans`), but this build has no Parquet extension
// available to test that leg against (`extension_config.cmake` loads only
// `chronoduck`), so it stays out of this rule's allow-list until #272 adds
// it with a real fixture — Article II.1's own "never claiming
// untested capability" posture, applied to an allow-list entry the same way
// it already applies to a registry.def row's edge modes. Every other table
// function — including a storage partner's own scan (e.g. RawDuck,
// `docs/testing/storage-partners.md`) — is declined for the same reason
// architecture.md's own Goal states: this project has no way to know from
// here whether an arbitrary table function's own filter pushdown actually
// *enforces* every filter it accepts rather than silently dropping it
// (RawDuck's own partial pushdown does exactly that for filters it doesn't
// understand), so pushing there could silently corrupt results — widening
// the scan (the safe failure this rule already risks everywhere else) is
// not the failure mode; dropping rows a residual filter was supposed to
// catch is.
bool IsPushableScanFunction(const string &name) {
	return name == "seq_scan";
}

// Resolves `binding` down through the linear chain rooted at `node` (i.e.
// `node`'s own single-child descendants) to the real scan `LogicalGet` that
// ultimately produces it — the pattern's own "[Filter] -> Get" half,
// generalized two ways real plans need: DuckDB's binder plans even a bare
// `ts_rate(ts, v, ...) FROM t` as `Aggregate -> Projection -> Get` (the
// projection evaluates every argument, constants included, before the
// aggregate runs), so a binding into an aggregate's own argument is only
// ever the *projection's* exposed column, never the Get's directly, and
// must be unwrapped one level through `expressions[binding.column_index]`
// (a plain column reference always names its expression's own child
// operator, never re-scoped to the projection itself — confirmed live
// against the built extension: an early version of this rule skipped this
// unwrap and never found a `Get` to push onto at all, see the PR's own
// Deviations section); and the operator form's synthetic passthrough Get
// (`LogicalGridStream`'s own header comment: "gives this node a
// one-column-passthrough LogicalGet child of its own ... rather than being
// that leaf itself") is a second, positional-identity level of the same
// kind of indirection. `LOGICAL_FILTER` carries no `table_index` of its own
// and is skipped without touching `binding` at all. Returns nullptr the
// moment `binding` names anything this pattern doesn't recognize — a join,
// an aggregate, a computed non-passthrough expression, or a chain that
// branches — decline is always the safe fallback, never a guess at an
// unfamiliar shape.
optional_ptr<LogicalGet> ResolveBindingToGet(LogicalOperator &node, ColumnBinding &binding) {
	if (node.children.size() != 1) {
		return nullptr;
	}
	auto &child = *node.children[0];
	if (child.type == LogicalOperatorType::LOGICAL_FILTER) {
		return ResolveBindingToGet(child, binding);
	}
	if (child.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = child.Cast<LogicalProjection>();
		if (projection.table_index != binding.table_index || binding.column_index >= projection.expressions.size()) {
			return nullptr;
		}
		auto &expr = *projection.expressions[binding.column_index];
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return nullptr;
		}
		binding = expr.Cast<BoundColumnRefExpression>().binding;
		return ResolveBindingToGet(child, binding);
	}
	if (child.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = child.Cast<LogicalGet>();
		if (get.table_index != binding.table_index) {
			return nullptr;
		}
		if (get.children.empty()) {
			return &get;
		}
		if (get.children.size() != 1 || get.function.name != kOperatorPassthroughFunctionName) {
			return nullptr;
		}
		auto child_bindings = get.children[0]->GetColumnBindings();
		if (binding.column_index >= child_bindings.size()) {
			return nullptr;
		}
		binding = child_bindings[binding.column_index];
		return ResolveBindingToGet(get, binding);
	}
	return nullptr;
}

// The one push site both plan shapes below share: resolves `ts_binding` down
// to the pattern's terminal `Get` (`ResolveBindingToGet`, which already
// guarantees the returned `Get`'s own `table_index` matches the final,
// rewritten `ts_binding`), checks it's a pushable function
// (`IsPushableScanFunction`) with no `projection_ids` remap in play, and — if
// every check holds — pushes `scan_bounds`'s two-sided bound as an ordinary
// `ConstantFilter` pair, exactly the shape DuckDB's own `FilterCombiner`
// would have pushed for a literal `ts BETWEEN ... AND ...` predicate (the
// vendored `duckdb` submodule's `src/optimizer/filter_combiner.cpp`,
// confirmed against that source rather than assumed).
void PushScanBound(LogicalOperator &top, ColumnBinding ts_binding, const Grid &grid, int64_t window) {
	auto get = ResolveBindingToGet(top, ts_binding);
	if (!get || !IsPushableScanFunction(get->function.name)) {
		return;
	}
	// `ts_binding.column_index` is a position among whatever `get` actually
	// exposes above itself — not cited in `file:construct:` form (the
	// `hygiene` CI job never checks out the vendored `duckdb` submodule): the
	// vendored submodule's own `LogicalGet::GetColumnBindings` builds that
	// exposed list by iterating `projection_ids` (when non-empty) rather than
	// `column_ids` directly, so `ts_binding.column_index` is a position into
	// `projection_ids`, not directly into `column_ids` — the two coincide
	// only when `projection_ids` is empty (nothing pushed down further).
	// Indexing `column_ids` by `ts_binding.column_index` directly here is
	// exactly the bug an early version of this rule had (confirmed live
	// against the built extension: it declined to push a bound at all,
	// because a plain `SELECT ts_rate(...) FROM t` already narrows the Get's
	// own exposed columns to just `ts`/`v`, so `projection_ids` is
	// essentially always non-empty in practice — see the PR's own Deviations
	// section).
	auto &column_ids = get->GetColumnIds();
	idx_t storage_pos = ts_binding.column_index;
	if (!get->projection_ids.empty()) {
		if (ts_binding.column_index >= get->projection_ids.size()) {
			return;
		}
		storage_pos = get->projection_ids[ts_binding.column_index];
	}
	if (storage_pos >= column_ids.size()) {
		return;
	}
	const ColumnIndex &storage_col = column_ids[storage_pos];

	auto bounds = chronoduck::scan_bounds(grid, window, /*lookback=*/0, chronoduck::EXTRAPOLATE);
	get->table_filters.PushFilter(storage_col, make_uniq<ConstantFilter>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	                                                                     Value::TIMESTAMP(timestamp_t(bounds.lower))));
	get->table_filters.PushFilter(storage_col, make_uniq<ConstantFilter>(ExpressionType::COMPARE_LESSTHANOREQUALTO,
	                                                                     Value::TIMESTAMP(timestamp_t(bounds.upper))));
}

// The Aggregate leg: pushes `agg`'s own single `ts_rate` call's bind-time
// grid/window, but ONLY when `agg` computes nothing else at all
// (`agg.expressions.size() == 1`). A `Get`'s `table_filters` apply to every
// row it emits regardless of which downstream expression reads it, so a
// sibling aggregate sharing this same `Get` (`SELECT ts_rate(narrow window),
// COUNT(*) FROM t`) or a second, differently-windowed `ts_rate` call
// (`SELECT ts_rate(w1), ts_rate(w2) FROM t`) would have its own input
// silently narrowed by a bound computed for the other call — confirmed live
// by the fresh-session review (round 2): the first repro's `COUNT(*)` came
// back wrong, and the second repro's second `ts_rate` call came back NULL,
// both traced directly to this rule's pushed filter (the PR's own Deviations
// section has the transcript). Requiring exactly one expression under `agg`
// is both necessary and sufficient to rule this out: DuckDB's plan here is a
// tree, not a DAG (`children` is a vector of `unique_ptr`s, so a `LogicalGet`
// object can have exactly one parent chain — the one construct that could
// make two parents share one `Get` instance, CTE materialization, produces
// `LogicalMaterializedCTE`/`LogicalCTERef` nodes this rule's own traversal
// already fails to recognize and therefore already declines), so the only
// other way a `Get` beneath this `agg` could have a second consumer is a
// sibling entry in `agg.expressions` itself. If its lone argument isn't a
// plain column reference (DuckDB already inserts an implicit cast around
// anything not already TIMESTAMP, which fails this check and correctly
// declines), this also declines.
void TryPushAggregateBound(LogicalAggregate &agg) {
	if (agg.expressions.size() != 1) {
		return;
	}
	auto &expr = agg.expressions[0];
	if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return;
	}
	auto &agg_expr = expr->Cast<BoundAggregateExpression>();
	if (agg_expr.function.name != "ts_rate" || !agg_expr.bind_info || agg_expr.children.empty()) {
		return;
	}
	auto &ts_expr = *agg_expr.children[0];
	if (ts_expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return;
	}
	auto &bind_data = agg_expr.bind_info->Cast<RateBindData>();
	PushScanBound(agg, ts_expr.Cast<BoundColumnRefExpression>().binding, bind_data.grid, bind_data.window);
}

// The ExtensionOperator leg: `stream_bind_data.ts_idx` names a position in
// the synthetic passthrough Get's own schema (`GridStreamBindOperator`'s own
// `bind_data.ts_idx = ts_idx;`, an index into `input.input_table_names`,
// exactly what that Get's own `col_types`/`names` are built from) — its own
// `GetColumnBindings()[ts_idx]` is the starting binding `ResolveBindingToGet`
// then unwraps down to the real Get's own storage column.
void TryPushOperatorBound(LogicalGridStream &grid_stream) {
	if (grid_stream.children.size() != 1) {
		return;
	}
	auto passthrough_bindings = grid_stream.children[0]->GetColumnBindings();
	if (grid_stream.stream_bind_data.ts_idx >= passthrough_bindings.size()) {
		return;
	}
	PushScanBound(grid_stream, passthrough_bindings[grid_stream.stream_bind_data.ts_idx],
	              grid_stream.stream_bind_data.grid, grid_stream.stream_bind_data.window);
}

// Walks the whole plan (not just its root) so a `ts_rate` call inside a
// subquery, CTE or one leg of a `UNION` is still found.
void ScanBoundPushdownVisit(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		TryPushAggregateBound(op.Cast<LogicalAggregate>());
	} else if (op.type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR && op.GetName() == "GRID_STREAM") {
		// `GetName()` is the only public, stable label a
		// `LogicalExtensionOperator` exposes to tell subclasses apart without
		// an unchecked `Cast` — safe here because "GRID_STREAM" is unique to
		// `LogicalGridStream` in this codebase (see its own `GetName()`).
		TryPushOperatorBound(op.Cast<LogicalGridStream>());
	}
	for (auto &child : op.children) {
		ScanBoundPushdownVisit(*child);
	}
}

void ScanBoundPushdownOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	Value setting_value;
	if (input.context.TryGetCurrentSetting(kScanBoundPushdownSetting, setting_value) &&
	    !setting_value.GetValue<bool>()) {
		return;
	}
	ScanBoundPushdownVisit(*plan);
}

void RegisterScanBoundPushdown(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(kScanBoundPushdownSetting,
	                          "Push ts_rate's scan bound onto the underlying scan as an ordinary table "
	                          "filter (T2.3); disabling it is only for the pushdown rule's own sentinel "
	                          "test, which asserts rows scanned grows by at least 10x without it",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	OptimizerExtension ext;
	ext.optimize_function = ScanBoundPushdownOptimize;
	OptimizerExtension::Register(config, ext);
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

	// The operator form (T2.2): a second, table-function catalog entry under
	// the same name — see the block comment above `RangeStreamBindData` for
	// why this needs no new `registry.def` row.
	loader.RegisterFunction(MakeTsRateOperatorTableFunction());
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
	// Not a registry.def row: an optimizer rule, not a SQL-visible function
	// (T2.3's own scope note — Article V.1 fences functions, and this adds
	// none).
	RegisterScanBoundPushdown(loader);
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
