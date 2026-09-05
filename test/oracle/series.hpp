// series.hpp — the L3 property sweep's own random-series/grid/window
// generators and structural sample types (docs/testing/layers.md's L3 row:
// "RapidCheck/FuzzTest sweeps of generated series x grids x windows x
// modes"). Part of `test/oracle/`, the from-scratch oracle AGENTS.md's
// "Where things are" section pins to "must never include `src/`" — this file
// includes nothing from this repository at all beyond the C++ standard
// library, so the fence (`scripts/hygiene/oracle-fence.py`, T5) has nothing
// to walk into here.
//
// `OracleSample`/`OracleGrid` are this directory's OWN sample/grid types —
// deliberately not `chronoduck::Sample`/`chronoduck::Grid`
// (`src/kernel/sample_buffer.hpp`, `src/kernel/grid.hpp`): sharing a type
// with the kernel would be a silent one-symbol coupling between an
// independent oracle and the thing it certifies, exactly what T5 forbids.
// `test/kernel/oracle_sweep_test.cpp` (which MAY include `src/`) is the only
// place these two independent worlds meet, converting one to the other in a
// few lines rather than sharing a struct.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace chronoduck::oracle {

// One (timestamp, value[, start-timestamp]) reading — the same fields
// `docs/testing/registry-and-fixtures.md`'s fixture format names (`(t, v)`
// or `(t, v, st)`), spelled independently here.
struct OracleSample {
	int64_t t = 0;
	double v = 0.0;
	bool has_st = false;
	int64_t st = 0;
};

// A regular grid of evaluation instants `start, start+step, ..., end`,
// independently spelled from `chronoduck::Grid` (`src/kernel/grid.hpp`).
struct OracleGrid {
	int64_t start = 0;
	int64_t end = 0;
	int64_t step = 1;

	int64_t count() const {
		return (end - start) / step + 1;
	}
	int64_t at(int64_t i) const {
		return start + i * step;
	}
};

// A tiny, dependency-free splitmix64 generator (Vigna's public-domain
// construction) — this file's only source of randomness, so every sweep run
// is reproducible from one committed seed with no external RNG dependency
// (no `<random>` engine whose bit-stream isn't part of any language
// standard).
class SplitMix64 {
public:
	explicit SplitMix64(uint64_t seed) : state_(seed) {
	}

	uint64_t Next() {
		uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return z ^ (z >> 31);
	}

	// Uniform in `[lo, hi]` inclusive, `hi >= lo`.
	int64_t UniformInt(int64_t lo, int64_t hi) {
		uint64_t span = static_cast<uint64_t>(hi - lo) + 1;
		return lo + static_cast<int64_t>(Next() % span);
	}

	// Uniform double in `[lo, hi)`.
	double UniformDouble(double lo, double hi) {
		double unit = static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0); // 53 bits
		return lo + unit * (hi - lo);
	}

	bool CoinFlip() {
		return (Next() & 1) == 1;
	}

private:
	uint64_t state_;
};

// A strictly increasing (t, v) counter series with no resets and no
// duplicate timestamps — every relation below builds its own variant from
// one of these, so the "true" shape (a real, monotone counter) lives in
// exactly one generator. Values start from a baseline large enough
// (`kBaselineMin`) relative to the per-step increments (`kStepMax`) that
// `MakeReset`'s rebase transform (below) always produces a genuine value
// drop, whatever split point it is given.
constexpr double kBaselineMin = 1000.0;
constexpr double kBaselineMax = 5000.0;
constexpr double kStepMin = 1.0;
constexpr double kStepMax = 50.0;

inline std::vector<OracleSample> MakeMonotoneSeries(SplitMix64 &rng, std::size_t n, int64_t t0, int64_t min_gap,
                                                    int64_t max_gap) {
	std::vector<OracleSample> out;
	out.reserve(n);
	int64_t t = t0;
	double v = rng.UniformDouble(kBaselineMin, kBaselineMax);
	for (std::size_t i = 0; i < n; i++) {
		out.push_back({t, v, false, 0});
		t += rng.UniformInt(min_gap, max_gap);
		v += rng.UniformDouble(kStepMin, kStepMax);
	}
	return out;
}

// Rebases every sample from `k` onward by subtracting `base[k-1].v` — a real
// counter reset ("the meter restarted and kept counting") that leaves the
// reset-adjusted delta `last - first` invariant (MR-RESET's own reason to
// exist: `counter_fold`'s reset branch adds the pre-reset value back in
// exactly once, so `sum(real increments)` is unchanged by where the "seam"
// falls). Requires `1 <= k < base.size()`, and relies on `kBaselineMin`
// staying comfortably above `kStepMax * base.size()` so `base[k] -
// base[k-1] < base[k-1]` always holds (a genuine drop, never an accidental
// non-reset).
inline std::vector<OracleSample> MakeReset(const std::vector<OracleSample> &base, std::size_t k) {
	std::vector<OracleSample> out = base;
	double offset = base[k - 1].v;
	for (std::size_t i = k; i < out.size(); i++) {
		out[i].v -= offset;
	}
	return out;
}

// A genuine value-reset variant of `base` that ends at the SAME last value
// as `base` itself: `base[0..k-1]` unchanged, a dip below `base[k-1]` at
// index `k` (a real drop), then a straight-line recovery back up to
// `base[n-1]` exactly by the last index. Requires `1 <= k <= n - 2` (at
// least one point strictly between the dip and the end to recover across).
//
// Distinct from `MakeReset` above on purpose: `MakeReset`'s constant-offset
// rebase changes the series' OWN last value (to `base[n-1] - base[k-1]`),
// which is exactly right for MR-RESET (comparing against the unmodified
// `base`, where the algebra cancels — see that relation's own header
// comment in `metamorphic.hpp`) but wrong for MR-ST, which compares against
// an UNMODIFIED, un-rebased monotone series and therefore needs a value
// reset variant that closes on that same unmodified final value instead.
inline std::vector<OracleSample> MakeDipAndRecover(const std::vector<OracleSample> &base, std::size_t k) {
	std::vector<OracleSample> out = base;
	double dip = base[k - 1].v * 0.3; // strictly below base[k-1].v: a genuine drop
	double last = base.back().v;
	std::size_t n = base.size();
	for (std::size_t i = k; i + 1 < n; i++) {
		double frac = static_cast<double>(i - k) / static_cast<double>(n - 1 - k);
		out[i].v = dip + (last - dip) * frac; // frac == 0 at i==k (== dip)
	}
	out[n - 1].v = last; // set directly, never through the interpolation formula: MR-ST's own invariance
	                     // proof needs this bit-identical to `base.back().v`, not merely "close" to it —
	                     // `dip + (last - dip) * 1.0` is mathematically `last` but not always bit-identical
	                     // to it under IEEE 754 rounding.
	return out;
}

// A Fisher-Yates shuffle over a copy of `data`, for MR-PERM.
inline std::vector<OracleSample> Shuffled(const std::vector<OracleSample> &data, SplitMix64 &rng) {
	std::vector<OracleSample> out = data;
	for (std::size_t i = out.size(); i-- > 1;) {
		std::size_t j = static_cast<std::size_t>(rng.UniformInt(0, static_cast<int64_t>(i)));
		std::swap(out[i], out[j]);
	}
	return out;
}

// Splits `data` into `parts` arbitrarily-sized, arbitrarily-ordered groups
// (each itself internally shuffled), then concatenates the groups back in a
// shuffled order — a different scramble shape than `Shuffled` above (whole
// flat shuffle) so MR-PART exercises a distinct code path than MR-PERM even
// though, for this composition (a single `SampleBuffer::sort_dedup` ahead of
// the fold, see `metamorphic.hpp`'s own MR-PART header comment), both
// reduce to the same "any permutation of the input rows" invariant.
inline std::vector<OracleSample> Partitioned(const std::vector<OracleSample> &data, std::size_t parts,
                                             SplitMix64 &rng) {
	if (parts < 2 || data.size() < parts) {
		return Shuffled(data, rng);
	}
	std::vector<std::vector<OracleSample>> groups(parts);
	for (std::size_t i = 0; i < data.size(); i++) {
		std::size_t g = static_cast<std::size_t>(rng.UniformInt(0, static_cast<int64_t>(parts - 1)));
		groups[g].push_back(data[i]);
	}
	std::vector<std::size_t> order(parts);
	for (std::size_t i = 0; i < parts; i++) {
		order[i] = i;
	}
	for (std::size_t i = order.size(); i-- > 1;) {
		std::size_t j = static_cast<std::size_t>(rng.UniformInt(0, static_cast<int64_t>(i)));
		std::swap(order[i], order[j]);
	}
	std::vector<OracleSample> out;
	out.reserve(data.size());
	for (std::size_t idx : order) {
		std::vector<OracleSample> shuffled_group = Shuffled(groups[idx], rng);
		out.insert(out.end(), shuffled_group.begin(), shuffled_group.end());
	}
	return out;
}

// The eval-instant sweep this issue's Goal names verbatim: `T ∈ {0, 15, 30,
// 60, 90, 120, 180, 300}` seconds off the sample clock — here spelled in the
// same abstract tick unit every other file in this kernel uses
// (`docs/design/architecture.md:time-native:` ticks are the caller's own
// unit; DuckDB's own caller feeds microseconds). `tick_scale` lets a caller
// treat one unit as a second (`tick_scale = 1`) or a microsecond
// (`tick_scale = 1` again, with `base_t` already in microseconds) — the
// offsets themselves are always exactly `{0,15,30,60,90,120,180,300} *
// tick_scale` ticks off `base_t`, a superset grid the same way
// `test/fixtures/rate/sweep-eval-instant-offsets.yaml`'s own 15-tick-step
// grid is a superset of the eight named offsets.
inline constexpr int64_t kEvalInstantOffsetsSeconds[] = {0, 15, 30, 60, 90, 120, 180, 300};
constexpr std::size_t kEvalInstantOffsetCount =
    sizeof(kEvalInstantOffsetsSeconds) / sizeof(kEvalInstantOffsetsSeconds[0]);

// A grid whose step is the GCD-friendly 15 (every named offset is a multiple
// of 15), running from `base_t` to `base_t + 300 * tick_scale` inclusive —
// every named offset is exactly one of this grid's points, and the grid also
// carries the ones the fixture's own comment calls "both sides agreeing on
// empty" (300 pushes the window off any data anchored near `base_t`).
inline OracleGrid EvalInstantOffsetGrid(int64_t base_t, int64_t tick_scale) {
	return OracleGrid {base_t, base_t + 300 * tick_scale, 15 * tick_scale};
}

} // namespace chronoduck::oracle
