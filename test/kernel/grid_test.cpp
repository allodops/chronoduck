// grid_test.cpp — the L1a direct test for `src/kernel/grid.hpp`
// (docs/testing/layers.md's L1a row: "every Tier 0-5 primitive has its own
// translation unit, its own table-driven tests ... exercised directly").
// Hand-rolled `main()`, no test framework, compiled and run with a bare
// `g++ -std=c++17` by `scripts/hygiene/kernel-primitive-tests.py` — the
// same dependency-free-TU pattern `kahan_test.cpp` established.
//
// Structure follows `docs/testing/primitives.md`'s Tier 1 `Grid` row: the
// unit contract (1/2/10^6-point grids, both construction errors, a
// microsecond-scale and a second-scale grid), the two stated invariants —
// `at(index_of(t)) <= t < at(index_of(t)+1)` (this issue's own acceptance
// criterion) and `index_of(at(i)) == i` — checked against an independent
// integer-arithmetic oracle, and the three named must-die mutants
// (floor<->ceil, +/-1 on count, inclusive<->exclusive end), each reproduced
// as a small shadow implementation carrying exactly that mutation.
#include "kernel/grid.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>

using chronoduck::Grid;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		std::fprintf(stderr, "grid_test: FAIL — %s\n", what);
		g_failures++;
	}
}

// The independent oracle for `index_of`: plain `long double` floor division,
// a genuinely different code path from `grid.hpp`'s widened `__int128_t`
// integer floor-div (docs/testing/primitives.md's own column: "Integer
// arithmetic in the test" — done here in a wider floating type instead of
// re-deriving the same integer algorithm).
int64_t OracleIndexOf(int64_t t, int64_t start, int64_t step) {
	long double offset = static_cast<long double>(t) - static_cast<long double>(start);
	long double q = offset / static_cast<long double>(step);
	return static_cast<int64_t>(std::floor(q));
}

void TestConstructionErrors() {
	// step <= 0 -> error.
	bool threw = false;
	try {
		Grid bad(0, 100, 0);
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	Check(threw, "step == 0 must throw a construction error");

	threw = false;
	try {
		Grid bad(0, 100, -5);
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	Check(threw, "negative step must throw a construction error");

	// end < start -> error.
	threw = false;
	try {
		Grid bad(100, 0, 10);
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	Check(threw, "end < start must throw a construction error");

	// step not dividing (end - start) -> error.
	threw = false;
	try {
		Grid bad(0, 100, 30); // 100 is not a multiple of 30
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	Check(threw, "a step that does not evenly divide end - start must throw a construction error");

	// A well-formed grid must not throw.
	threw = false;
	try {
		Grid ok(0, 100, 10);
		(void)ok;
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	Check(!threw, "a well-formed grid must not throw");
}

// Unit contract: grids with 1, 2, and 10^6 points.
void TestCountAtIndexOfOnSizedGrids() {
	{
		Grid g(50, 50, 10); // start == end: exactly one point
		Check(g.count() == 1, "a 1-point grid (start == end) must have count() == 1");
		Check(g.at(0) == 50, "a 1-point grid's only point must be at start");
		Check(g.index_of(50) == 0, "index_of(start) on a 1-point grid must be 0");
	}
	{
		Grid g(0, 10, 10); // two points: 0, 10
		Check(g.count() == 2, "a 2-point grid must have count() == 2");
		Check(g.at(0) == 0 && g.at(1) == 10, "a 2-point grid's points must be {start, end}");
		Check(g.index_of(0) == 0 && g.index_of(10) == 1, "index_of at each of the 2 points must match");
	}
	{
		const int64_t step = 1000; // 1 ms in microseconds
		const int64_t n = 1000000;
		Grid g(0, step * (n - 1), step); // exactly 10^6 points
		Check(g.count() == n, "a 10^6-point grid must report count() == 10^6");
		Check(g.at(0) == 0, "the first point of a 10^6-point grid must be start");
		Check(g.at(n - 1) == g.end, "the last point of a 10^6-point grid must be end");
		Check(g.index_of(g.end) == n - 1, "index_of(end) on a 10^6-point grid must be count() - 1");
	}
}

// Unit contract: microsecond vs. second inputs — the same primitive must
// behave identically regardless of the tick scale a caller feeds it.
void TestMicrosecondVsSecondScale() {
	Grid micro(0, 5000000, 1000000); // 5 points, 1s step, expressed in microseconds
	Grid second_scale(0, 5, 1);      // the same 5 points, expressed directly in "seconds"
	Check(micro.count() == second_scale.count(),
	      "microsecond- and second-scale grids of the same shape must agree on count()");
	for (int64_t i = 0; i < micro.count(); i++) {
		Check(micro.index_of(micro.at(i)) == second_scale.index_of(second_scale.at(i)),
		      "index_of(at(i)) must agree across tick scales at every grid point");
	}
}

// AC1 (this issue's own acceptance criterion): "at(index_of(t)) <= t <
// at(index_of(t)+1) property" — exercised for random t across several
// grids, including negative starts (t before the grid) and t past the
// grid's declared end, since the invariant is stated for "random t", not
// only in-range t.
void TestAtIndexOfInvariantForRandomT() {
	struct GridSpec {
		int64_t start, end, step;
	};
	const GridSpec specs[] = {
	    {0, 1000, 10},
	    {-500, 494, 7}, // negative start, a step that isn't a power of 2
	    {1000000, 1000000 + 999 * 3, 3},
	    {-1000000000, -999999000, 1000},
	};

	std::mt19937_64 rng(0xC0FFEE);
	for (const auto &spec : specs) {
		Grid g(spec.start, spec.end, spec.step);
		// Random t spanning well before the grid to well past it, not just
		// the grid's own [start, end] span.
		int64_t span = spec.end - spec.start;
		std::uniform_int_distribution<int64_t> dist(spec.start - 2 * span - 1, spec.end + 2 * span + 1);
		for (int trial = 0; trial < 2000; trial++) {
			int64_t t = dist(rng);
			int64_t idx = g.index_of(t);
			int64_t oracle_idx = OracleIndexOf(t, spec.start, spec.step);
			Check(idx == oracle_idx, "index_of(t) must match the independent long-double floor oracle");
			Check(g.at(idx) <= t, "at(index_of(t)) <= t must hold for random t");
			Check(t < g.at(idx + 1), "t < at(index_of(t) + 1) must hold for random t");
		}
	}
}

// Invariant: index_of(at(i)) == i.
void TestIndexOfAtInvariant() {
	Grid g(-100, 900, 20);
	for (int64_t i = 0; i < g.count(); i++) {
		Check(g.index_of(g.at(i)) == i, "index_of(at(i)) must equal i for every valid grid index");
	}
}

// A regression test for a real bug found in this project's own M1 ACPR
// (Adversarial Critic Pass Review): the constructor's span-divisibility
// check and `count()` computed `end - start` as plain `int64_t`, unlike
// `index_of()`, which was already correctly widened to `__int128_t`
// (`docs/design/architecture.md:time-native:` cites the same argument —
// two legal, in-range DuckDB `TIMESTAMP`s can be ~1.85e19 microseconds
// apart, beyond `int64_t`'s ~9.22e18 max). That gap was reachable from
// ordinary SQL (`BindGridArgs` passes bind-time `start`/`end` straight into
// `Grid(...)`) as signed-integer-overflow UB, and this exact test file had
// no case covering it — the ACPR's own "we are blind in test" finding.
// This test both proves the fix's correctness on a legal extreme-span grid
// and, via a must-die shadow mutant reproducing the original unwidened
// arithmetic, proves the fix is load-bearing (the mutant produces a visibly
// wrong, wrapped-around count on the same input, not just "compiles").
void TestExtremeSpanOverflow() {
	// A legal grid whose span exceeds int64_t's range: start near INT64_MIN,
	// step large enough that count() stays representable while (end - start)
	// itself does not fit in int64_t.
	const int64_t start = INT64_MIN + 500;
	const int64_t step = 1000000000LL; // 1000s in microseconds
	const int64_t n = 18000000001LL;   // grid point count (chosen so the span is ~1.8e19 us > INT64_MAX)
	const int64_t end = static_cast<int64_t>(static_cast<__int128_t>(start) + static_cast<__int128_t>(n - 1) * step);

	bool threw = false;
	Grid *g = nullptr;
	try {
		g = new Grid(start, end, step);
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	Check(!threw, "a legal extreme-span grid (end - start > INT64_MAX) must not throw a spurious construction error");
	if (g != nullptr) {
		Check(g->count() == n,
		      "count() on an extreme-span grid must report the correct value, not a wrapped-around one");
		Check(g->at(0) == start, "at(0) on an extreme-span grid must be start");
		Check(g->index_of(end) == n - 1, "index_of(end) on an extreme-span grid must be count() - 1");
		delete g;
	}
}

// Must-die mutant: the original unwidened `int64_t` arithmetic this bug
// fix replaced. Reproduces the exact plain-`int64_t` `(end - start)`
// computation the real constructor/`count()` used before the fix, on the
// same extreme-span input `TestExtremeSpanOverflow` uses — the wraparound
// this mutant exhibits (a negative or otherwise nonsensical span) is what
// the real code no longer does.
int64_t MutantUnwidenedCount(int64_t start, int64_t end, int64_t step) {
	return (end - start) / step + 1; // the pre-fix, unwidened computation
}

void TestUnwidenedSpanMutant() {
	const int64_t start = INT64_MIN + 500;
	const int64_t step = 1000000000LL;
	const int64_t n = 18000000001LL;
	const int64_t end = static_cast<int64_t>(static_cast<__int128_t>(start) + static_cast<__int128_t>(n - 1) * step);

	// Diffs against the REAL, live `Grid` instance's own `count()` — not
	// against a hand-computed constant — the same pattern every other
	// mutant test in this file follows (e.g. `TestCountOffByOneMutant`
	// diffs against `g.end`). If `Grid::count()` ever regresses back to
	// plain `int64_t` arithmetic, it would compute the identical wrapped
	// value this mutant computes, and this assertion — not just a
	// stand-alone constant comparison — is what catches that.
	//
	// Construction is wrapped in try/catch (unlike the file's other three
	// mutant tests, which construct a small, always-valid `Grid`): a
	// regression to unwidened arithmetic can corrupt the constructor's own
	// `span % step` divisibility check on this extreme-span input, throwing
	// instead of just miscounting — a broken build must still report a
	// clean `FAIL`, not crash the whole test binary via an uncaught
	// exception (confirmed live: reverting the fix crashes this test
	// uncaught before this try/catch was added).
	bool threw = false;
	int64_t real_count = 0;
	try {
		Grid g(start, end, step);
		real_count = g.count();
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	Check(!threw, "sanity: the real, fixed Grid must construct without throwing on this extreme-span input");
	Check(real_count == n,
	      "sanity: the real, fixed Grid's count() on an extreme-span grid must equal the correct value");
	int64_t mutant_count = MutantUnwidenedCount(start, end, step);
	Check(mutant_count != real_count,
	      "must-die: the unwidened-arithmetic mutant's count() on an extreme-span grid must differ from the real "
	      "Grid's own count() — if they match (or the real Grid threw above), Grid has regressed back to plain "
	      "int64_t arithmetic");
}

// Must-die mutant #1: "Floor<->ceil"
// (`docs/testing/primitives.md:grid-row:` `Floor↔ceil`). A shadow
// `index_of` that rounds up instead of down; correct on every t that lands
// exactly on a grid point, wrong (violates AC1) on every t strictly between
// two points.
int64_t MutantCeilIndexOf(int64_t t, int64_t start, int64_t step) {
	int64_t offset = t - start;
	int64_t q = offset / step;
	int64_t r = offset % step;
	if (r != 0 && offset > 0) {
		q++; // ceiling instead of floor
	}
	return q;
}

void TestFloorCeilMutant() {
	Grid g(0, 1000, 10);
	int64_t t = 25; // strictly between grid points 20 and 30
	int64_t real_idx = g.index_of(t);
	Check(g.at(real_idx) <= t && t < g.at(real_idx + 1), "sanity: the real index_of satisfies AC1 at t=25");

	int64_t mutant_idx = MutantCeilIndexOf(t, g.start, g.step);
	int64_t mutant_at = g.start + mutant_idx * g.step; // reuse Grid::at's own formula for the mutant's point
	Check(!(mutant_at <= t), "must-die: the ceil mutant's index_of(25) violates at(index_of(t)) <= t");
}

// Must-die mutant #2: "+/-1 on count"
// (`docs/testing/primitives.md:grid-row:` `±1 on count`). Dropping the `+ 1`
// undercounts by exactly one point, so the mutant's last point falls short
// of the grid's declared `end`.
int64_t MutantCountMinusOne(const Grid &g) {
	return (g.end - g.start) / g.step; // missing "+ 1"
}

void TestCountOffByOneMutant() {
	Grid g(0, 100, 10); // 11 real points: 0, 10, ..., 100
	Check(g.count() == 11, "sanity: the real grid has 11 points");
	int64_t mutant_count = MutantCountMinusOne(g);
	Check(mutant_count == 10, "the +/-1 mutant undercounts by exactly one");
	Check(g.at(mutant_count - 1) != g.end,
	      "must-die: the +/-1 mutant's last point (at(mutant_count-1)) does not reach the grid's declared end");
	Check(g.at(g.count() - 1) == g.end, "the real grid's last point does reach the declared end");
}

// Must-die mutant #3: "inclusive<->exclusive end"
// (`docs/testing/primitives.md:grid-row:` `inclusive↔exclusive end`). The
// real construction check is `end < start -> error`, which admits the
// legitimate 1-point grid `start == end`. A mutant validator that instead
// rejects `end <= start` treats `end` as an exclusive bound and wrongly
// flags that same 1-point grid as invalid.
bool MutantRejectsExclusiveEnd(int64_t start, int64_t end) {
	return end <= start; // flipped from the real "end < start"
}

void TestInclusiveExclusiveEndMutant() {
	bool threw = false;
	try {
		Grid single_point(50, 50, 10); // start == end: a legitimate 1-point grid
		(void)single_point;
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	Check(!threw, "sanity: the real Grid accepts the legitimate start == end, 1-point case");
	Check(MutantRejectsExclusiveEnd(50, 50),
	      "must-die: the exclusive-end mutant wrongly rejects the legitimate start == end, 1-point grid");
}

} // namespace

int main() {
	TestConstructionErrors();
	TestCountAtIndexOfOnSizedGrids();
	TestMicrosecondVsSecondScale();
	TestAtIndexOfInvariantForRandomT();
	TestIndexOfAtInvariant();
	TestExtremeSpanOverflow();
	TestUnwidenedSpanMutant();
	TestFloorCeilMutant();
	TestCountOffByOneMutant();
	TestInclusiveExclusiveEndMutant();

	if (g_failures > 0) {
		std::fprintf(stderr, "grid_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("grid_test: PASS\n");
	return 0;
}
