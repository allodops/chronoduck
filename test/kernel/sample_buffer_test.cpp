// sample_buffer_test.cpp — the L1a direct test for
// `src/kernel/sample_buffer.hpp` (docs/testing/layers.md's L1a row: "every
// Tier 0-5 primitive has its own translation unit, its own table-driven
// tests ... exercised directly"). Hand-rolled `main()`, no test framework,
// compiled and run with a bare `g++ -std=c++17` by
// `scripts/hygiene/kernel-primitive-tests.mjs` — the same dependency-free-TU
// pattern `kahan_test.cpp`/`window_test.cpp` established.
//
// Structure follows `docs/testing/primitives.md`'s Tier 2 `SampleBuffer` and
// `dedup_policy` rows: `append` growth across arena pages (with pointer
// stability, the property that makes this an arena rather than a
// `std::vector`); `merge` of empty/nonempty/both; `sort_dedup` on
// already-sorted, reverse, random and all-equal-timestamp inputs, checked
// against an independent `std::stable_sort` + `std::unique` oracle; `slice`
// at every boundary; the destructor's page release proven directly on the
// arena; and the must-die mutants named in both rows — a naive
// insertion-order-dependent "keep first" dedup (the "stable-sort" mutant
// this issue's own acceptance criterion names), "keep the minimum instead
// of the maximum", and "a missed page free" — each demonstrated by a
// shadow implementation carrying the mutation, the same standard
// `kahan_test.cpp` set for this repo.
#include "../../src/kernel/sample_buffer.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

using chronoduck::dedup_policy;
using chronoduck::Sample;
using chronoduck::SampleBuffer;
using chronoduck::SampleSpan;
using chronoduck::stale_marker;
using chronoduck::detail::SampleArena;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		std::fprintf(stderr, "sample_buffer_test: FAIL — %s\n", what);
		g_failures++;
	}
}

uint64_t Bits(double v) {
	uint64_t bits;
	std::memcpy(&bits, &v, sizeof(bits));
	return bits;
}

bool BitExact(double a, double b) {
	return Bits(a) == Bits(b);
}

// ---------------------------------------------------------------------
// The independent oracle: `docs/testing/primitives.md`'s own column for
// this row — `std::stable_sort` + `std::unique` with the tie-break, on a
// copy. `std::unique`'s merging predicate folds `dedup_policy` pairwise
// across each run of equal timestamps, a different STL mechanism from
// `sample_buffer.hpp`'s own explicit-loop `sort_dedup`, so this genuinely
// cross-checks a second code path rather than re-running the same one.
// ---------------------------------------------------------------------
std::vector<Sample> OracleSortDedup(std::vector<Sample> samples) {
	std::stable_sort(samples.begin(), samples.end(), [](const Sample &a, const Sample &b) { return a.t < b.t; });
	auto merge_duplicate = [](Sample &kept, const Sample &next) {
		if (kept.t != next.t)
			return false;
		if (dedup_policy(next.v, kept.v)) {
			kept.v = next.v;
		}
		return true;
	};
	auto new_end = std::unique(samples.begin(), samples.end(), merge_duplicate);
	samples.erase(new_end, samples.end());
	return samples;
}

// Folds a timestamp group with an arbitrary policy function, the same shape
// `sort_dedup`'s own group scan uses — lets the must-die mutants below be
// exercised directly, isolated from sorting.
double FoldGroupBestOf(const std::vector<double> &values, bool (*policy)(double, double)) {
	double best = values.front();
	for (std::size_t i = 1; i < values.size(); i++) {
		if (policy(values[i], best)) {
			best = values[i];
		}
	}
	return best;
}

// Must-die mutant: "keep the minimum instead of the maximum"
// (`docs/testing/primitives.md:dedup-policy-row:` `Max↔min↔first↔last`).
// Reverses the rank comparison `dedup_policy` itself makes
// (`src/kernel/sample_buffer.hpp:dedup_policy:` `detail::RankLess(detail::RankOf(incumbent), detail::RankOf(challenger))`),
// so the "winner" is the least-ranked candidate instead of the greatest.
bool MutantDedupPolicyKeepsMin(double challenger, double incumbent) {
	return chronoduck::detail::RankLess(chronoduck::detail::RankOf(challenger), chronoduck::detail::RankOf(incumbent));
}

// Must-die mutant: "keep whichever came first" — the other half of
// `Max↔min↔first↔last`, and the exact shape of this issue's own "stable-sort"
// acceptance criterion. Sorts by timestamp only and keeps the first of each
// run, never consulting `dedup_policy` at all — so its answer for a
// duplicate-timestamp group depends on *insertion order*, not on the
// declared value-based policy.
std::vector<Sample> MutantKeepFirstOfGroup(std::vector<Sample> samples) {
	std::stable_sort(samples.begin(), samples.end(), [](const Sample &a, const Sample &b) { return a.t < b.t; });
	auto same_timestamp = [](const Sample &a, const Sample &b) { return a.t == b.t; };
	auto new_end = std::unique(samples.begin(), samples.end(), same_timestamp);
	samples.erase(new_end, samples.end());
	return samples;
}

// Must-die mutant: "a missed page free"
// (`docs/testing/primitives.md:samplebuffer-row:` `in dedup; a missed page free`).
// A standalone shadow of the arena's page bookkeeping whose release loop
// stops one page short — a realistic off-by-one, not a hypothetical. The
// "leaked" page's pointer is captured before the buggy release clears it and
// is freed for real immediately after the check, so this demonstration
// itself stays leak-clean under ASan; only the *counter*, not real memory,
// is left to prove the point.
struct LeakyPageArena {
	struct Page {
		Sample *data;
		std::size_t capacity;
	};
	std::vector<Page> pages;
	std::size_t bytes = 0;

	void add_page(std::size_t capacity) {
		auto *data = static_cast<Sample *>(::operator new(capacity * sizeof(Sample)));
		bytes += capacity * sizeof(Sample);
		pages.push_back({data, capacity});
	}

	void leaky_release_all() {
		for (std::size_t i = 0; i + 1 < pages.size(); i++) { // bug: should be `i < pages.size()`
			bytes -= pages[i].capacity * sizeof(Sample);
			::operator delete(pages[i].data);
		}
		pages.clear();
	}
};

// ---------------------------------------------------------------------
// `append` growth across arena pages, with pointer stability.
// ---------------------------------------------------------------------
void TestAppendGrowsAcrossPages() {
	SampleArena arena;
	std::vector<Sample *> early_pointers;
	const int kFirstPageCapacity = 4;
	for (int i = 0; i < kFirstPageCapacity; i++) {
		Sample *s = arena.push();
		s->t = i;
		s->v = i * 1.5;
		early_pointers.push_back(s);
	}
	Check(arena.page_count() == 1, "exactly one page's worth of pushes must not yet grow");

	for (int i = kFirstPageCapacity; i < 100; i++) {
		Sample *s = arena.push();
		s->t = i;
		s->v = i * 1.5;
	}
	Check(arena.page_count() > 1, "growth past the first page's capacity must add more pages");
	Check(arena.size() == 100, "the arena must report every pushed sample, across every page");

	// Pointer stability: pointers handed out by the earliest pushes must
	// still read back correctly after many subsequent pushes forced several
	// more page allocations — a `std::vector`'s reallocate-and-copy growth
	// would have invalidated these; a page-based arena does not.
	for (int i = 0; i < kFirstPageCapacity; i++) {
		char what[128];
		std::snprintf(what, sizeof(what), "pointer from push #%d must survive later page growth unmoved", i);
		Check(early_pointers[static_cast<std::size_t>(i)]->t == i && early_pointers[static_cast<std::size_t>(i)]->v == i * 1.5,
		      what);
	}
}

// ---------------------------------------------------------------------
// `merge` of empty/nonempty/both.
// ---------------------------------------------------------------------
void TestMergeEmptyNonemptyBoth() {
	{
		SampleBuffer a, b;
		a.merge(b);
		Check(a.size() == 0, "merging two empty buffers stays empty");
	}
	{
		SampleBuffer a, b;
		b.append(1, 10.0);
		b.append(2, 20.0);
		a.merge(b);
		Check(a.size() == 2, "merging a nonempty buffer into an empty one adopts its samples");
		Check(b.size() == 2, "merge must not mutate the source buffer");
	}
	{
		SampleBuffer a, b;
		a.append(1, 10.0);
		a.merge(b);
		Check(a.size() == 1, "merging an empty buffer into a nonempty one is a no-op");
	}
	{
		SampleBuffer a, b;
		a.append(1, 10.0);
		b.append(2, 20.0);
		b.append(3, 30.0);
		a.merge(b);
		Check(a.size() == 3, "merging two nonempty buffers concatenates their raw samples");
	}
}

// ---------------------------------------------------------------------
// `sort_dedup` on already-sorted, reverse, random and all-equal-timestamp
// inputs, checked against the independent oracle, plus this row's own
// invariants (strictly increasing timestamps; length <= input).
// ---------------------------------------------------------------------
void CheckSortDedupMatchesOracle(const std::vector<Sample> &raw, const char *label) {
	SampleBuffer buf;
	for (const Sample &s : raw) {
		buf.append(s);
	}
	buf.sort_dedup();

	std::vector<Sample> expected = OracleSortDedup(raw);

	char what[256];
	std::snprintf(what, sizeof(what), "%s: sort_dedup size (%zu) must match the independent oracle's (%zu)", label,
	              buf.size(), expected.size());
	Check(buf.size() == expected.size(), what);
	Check(buf.size() <= raw.size(), "invariant: sort_dedup's length must never exceed the input length");

	SampleSpan got = buf.slice(std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
	std::size_t n = std::min(got.size(), expected.size());
	for (std::size_t i = 0; i < n; i++) {
		std::snprintf(what, sizeof(what),
		              "%s: sample %zu must match the oracle bit-exact (t=%lld got.v=%.17g expected.v=%.17g)", label, i,
		              static_cast<long long>(expected[i].t), got.begin[i].v, expected[i].v);
		Check(got.begin[i].t == expected[i].t && BitExact(got.begin[i].v, expected[i].v), what);
	}
	for (std::size_t i = 1; i < got.size(); i++) {
		Check(got.begin[i].t > got.begin[i - 1].t, "invariant: timestamps must be strictly increasing after sort_dedup");
	}
}

void TestSortDedupOrderings() {
	std::vector<Sample> sorted_input = {{10, 1.0}, {20, 2.0}, {30, 3.0}, {30, 30.0}, {40, 4.0}};
	CheckSortDedupMatchesOracle(sorted_input, "already-sorted");

	std::vector<Sample> reverse_input = {{40, 4.0}, {30, 30.0}, {30, 3.0}, {20, 2.0}, {10, 1.0}};
	CheckSortDedupMatchesOracle(reverse_input, "reverse");

	std::mt19937 rng(12345);
	std::uniform_int_distribution<int64_t> t_dist(0, 40); // narrow range: forces plenty of duplicates
	std::uniform_real_distribution<double> v_dist(-100.0, 100.0);
	std::vector<Sample> random_input;
	for (int i = 0; i < 200; i++) {
		random_input.push_back({t_dist(rng), v_dist(rng)});
	}
	CheckSortDedupMatchesOracle(random_input, "random");

	std::vector<Sample> all_equal_input;
	for (int i = 0; i < 50; i++) {
		all_equal_input.push_back({777, v_dist(rng)});
	}
	CheckSortDedupMatchesOracle(all_equal_input, "all-equal-timestamps");
}

// Invariant: "merge then sort_dedup equals sort_dedup of the concatenation,
// bit-exact."
void TestMergeThenSortDedupEqualsSortDedupOfConcatenation() {
	std::vector<Sample> part_a = {{1, 5.0}, {2, 6.0}, {3, 7.0}};
	std::vector<Sample> part_b = {{2, 60.0}, {3, 7.0}, {4, 8.0}}; // overlaps at 2 (differing value) and 3 (same value)

	SampleBuffer a, b;
	for (const Sample &s : part_a) {
		a.append(s);
	}
	for (const Sample &s : part_b) {
		b.append(s);
	}
	a.merge(b);
	a.sort_dedup();

	std::vector<Sample> concatenated = part_a;
	concatenated.insert(concatenated.end(), part_b.begin(), part_b.end());
	SampleBuffer c;
	for (const Sample &s : concatenated) {
		c.append(s);
	}
	c.sort_dedup();

	Check(a.size() == c.size(), "merge-then-sort_dedup size must equal sort_dedup-of-concatenation's size");
	SampleSpan sa = a.slice(std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
	SampleSpan sc = c.slice(std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
	std::size_t n = std::min(sa.size(), sc.size());
	for (std::size_t i = 0; i < n; i++) {
		Check(sa.begin[i].t == sc.begin[i].t && BitExact(sa.begin[i].v, sc.begin[i].v),
		      "merge-then-sort_dedup must equal sort_dedup-of-concatenation bit-exact, sample-by-sample");
	}
}

// ---------------------------------------------------------------------
// This issue's own acceptance criterion: "Stable-sort test only the
// tie-break can detect." Two insertion orders of the same {10.0, 90.0}
// pair at one timestamp must both converge on 90.0 under the real
// `sort_dedup` — and the naive "keep first" mutant above must NOT (it must
// disagree between the two orderings), which is exactly why a test needs a
// genuine differing-value duplicate to catch this class of bug at all: a
// same-value duplicate or a series with no duplicate timestamp would pass
// against the naive mutant too.
// ---------------------------------------------------------------------
void TestStableSortOnlyTieBreakCanDetect() {
	SampleBuffer order_a, order_b;
	order_a.append(500, 10.0);
	order_a.append(500, 90.0);
	order_b.append(500, 90.0);
	order_b.append(500, 10.0);

	order_a.sort_dedup();
	order_b.sort_dedup();

	SampleSpan a_result = order_a.slice(500, 500);
	SampleSpan b_result = order_b.slice(500, 500);
	Check(a_result.size() == 1 && a_result.begin->v == 90.0,
	      "sort_dedup must keep the greater value (90.0) regardless of insertion order (10.0 then 90.0)");
	Check(b_result.size() == 1 && b_result.begin->v == 90.0,
	      "sort_dedup must keep the greater value (90.0) regardless of insertion order (90.0 then 10.0)");

	std::vector<Sample> raw_a = {{500, 10.0}, {500, 90.0}};
	std::vector<Sample> raw_b = {{500, 90.0}, {500, 10.0}};
	std::vector<Sample> mutant_a = MutantKeepFirstOfGroup(raw_a);
	std::vector<Sample> mutant_b = MutantKeepFirstOfGroup(raw_b);
	Check(mutant_a.size() == 1 && mutant_a[0].v == 10.0,
	      "must-die: the naive keep-first mutant keeps whatever was inserted first (10.0) for this ordering");
	Check(mutant_b.size() == 1 && mutant_b[0].v == 90.0,
	      "must-die: the same mutant on the reversed insertion order keeps 90.0 instead — its answer depends on "
	      "insertion order, unlike the real sort_dedup above");
}

void TestMutantDedupPolicyKeepsMin() {
	Check(FoldGroupBestOf({10.0, 90.0}, dedup_policy) == 90.0, "the real dedup_policy must keep the maximum, 90.0");
	Check(FoldGroupBestOf({10.0, 90.0}, MutantDedupPolicyKeepsMin) == 10.0,
	      "must-die: reversing the rank comparison keeps the minimum (10.0) instead of the maximum");
}

// ---------------------------------------------------------------------
// This issue's own acceptance criterion: "−0/+0, NaN payloads, stale-vs-NaN
// rows" — the NaN/stale/±0 matrix ADR 0004 and
// `docs/design/architecture.md` declare, exercised directly against
// `dedup_policy`, both insertion orders, with bit-exact checks (`==` alone
// cannot distinguish −0 from +0 or one NaN payload from another).
// ---------------------------------------------------------------------
void TestSignedZeroTieBreak() {
	Check(dedup_policy(+0.0, -0.0), "+0.0 challenging -0.0 must win: -0 < +0 under totalOrder");
	Check(!dedup_policy(-0.0, +0.0), "-0.0 challenging +0.0 must lose: -0 < +0 under totalOrder");

	double first_then_second = FoldGroupBestOf({-0.0, +0.0}, dedup_policy);
	double second_then_first = FoldGroupBestOf({+0.0, -0.0}, dedup_policy);
	Check(BitExact(first_then_second, 0.0) && !BitExact(first_then_second, -0.0),
	      "the winner of a {-0.0, +0.0} group must be +0.0's exact bit pattern");
	Check(BitExact(first_then_second, second_then_first), "the winner must not depend on insertion order");
}

void TestNaNPayloadsTieBreak() {
	double nan_a, nan_b;
	uint64_t nan_a_bits = 0x7ff8000000000001ULL, nan_b_bits = 0x7ff8000000000005ULL;
	std::memcpy(&nan_a, &nan_a_bits, sizeof(nan_a));
	std::memcpy(&nan_b, &nan_b_bits, sizeof(nan_b));
	Check(std::isnan(nan_a) && std::isnan(nan_b), "sanity: both constructed payloads are NaN");
	Check(Bits(nan_a) != chronoduck::kStaleNaNBits && Bits(nan_b) != chronoduck::kStaleNaNBits,
	      "sanity: neither payload is the stale marker");

	double winner_ab = FoldGroupBestOf({nan_a, nan_b}, dedup_policy);
	double winner_ba = FoldGroupBestOf({nan_b, nan_a}, dedup_policy);
	Check(std::isnan(winner_ab) && !chronoduck::is_stale(winner_ab),
	      "the winner of two distinct ordinary NaN payloads must still be an ordinary NaN");
	Check(BitExact(winner_ab, winner_ba), "the winning NaN payload must not depend on insertion order");

	// NaN loses to non-NaN, either direction.
	Check(dedup_policy(5.0, nan_a), "a real value challenging an ordinary NaN must win: NaN loses to non-NaN");
	Check(!dedup_policy(nan_a, 5.0), "an ordinary NaN challenging a real value must lose: NaN loses to non-NaN");
}

void TestStaleVsNaNTieBreak() {
	double stale = stale_marker();
	double ordinary_nan = 0.0 / 0.0;
	Check(chronoduck::is_stale(stale) && std::isnan(ordinary_nan) && !chronoduck::is_stale(ordinary_nan),
	      "sanity: stale is the stale marker, ordinary_nan is a plain NaN");

	double winner_1 = FoldGroupBestOf({stale, ordinary_nan}, dedup_policy);
	double winner_2 = FoldGroupBestOf({ordinary_nan, stale}, dedup_policy);
	Check(std::isnan(winner_1) && !chronoduck::is_stale(winner_1),
	      "the stale marker must lose to an ordinary NaN (order: stale, then ordinary)");
	Check(std::isnan(winner_2) && !chronoduck::is_stale(winner_2),
	      "the stale marker must lose to an ordinary NaN (order: ordinary, then stale)");

	// Stale also loses to any real value.
	double winner_vs_real = FoldGroupBestOf({stale, 5.0}, dedup_policy);
	Check(winner_vs_real == 5.0, "the stale marker must lose to a real value too");
}

// ---------------------------------------------------------------------
// `slice` at every boundary.
// ---------------------------------------------------------------------
void TestSliceAtEveryBoundary() {
	SampleBuffer buf;
	buf.append(10, 1.0);
	buf.append(20, 2.0);
	buf.append(30, 3.0);
	buf.sort_dedup();

	Check(buf.slice(0, 5).size() == 0, "before every sample: empty");
	Check(buf.slice(10, 10).size() == 1 && buf.slice(10, 10).begin->t == 10,
	      "closed at the first sample's own timestamp");
	Check(buf.slice(9, 10).size() == 1, "lo just before the first, hi at the first: includes exactly the first");
	Check(buf.slice(10, 20).size() == 2, "covering the first two exactly at their timestamps");
	Check(buf.slice(11, 19).size() == 0, "strictly between two samples: empty");
	Check(buf.slice(30, 30).size() == 1 && buf.slice(30, 30).begin->t == 30,
	      "closed at the last sample's own timestamp");
	Check(buf.slice(30, 100).size() == 1, "hi past the last sample: includes just the last");
	Check(buf.slice(100, 200).size() == 0, "after every sample: empty");
	Check(buf.slice(10, 30).size() == 3, "the full range, both ends inclusive");
	Check(buf.slice(5, 100).size() == 3, "a range dwarfing the buffer: everything");

	SampleBuffer empty;
	empty.sort_dedup();
	Check(empty.slice(0, 100).size() == 0, "slicing an empty (but sorted) buffer must not crash and must be empty");
}

// ---------------------------------------------------------------------
// Destructor releases every page (ASan + the allocator's byte counter
// returns to zero), and its must-die mutant, "a missed page free."
//
// The destructor's entire job is calling the same `release_all` this test
// exercises directly via `compact_to({})` (`sort_dedup`'s own compaction
// step calls it too, before adding the replacement page) — proving that
// call zeroes the counter is proving what `~SampleArena` does, without
// needing to read a destroyed object's memory (undefined behaviour) to
// observe it.
// ---------------------------------------------------------------------
void TestArenaDestructorReleasesEveryPage() {
	SampleArena arena;
	for (int i = 0; i < 100; i++) {
		Sample *s = arena.push();
		s->t = i;
		s->v = static_cast<double>(i);
	}
	Check(arena.page_count() > 1, "100 pushes with a doubling page size starting at 4 must span multiple pages");
	Check(arena.byte_count() > 0, "the byte counter must be nonzero while pages are live");

	arena.compact_to({}); // the destructor's own release path, with nothing to replace the released pages
	Check(arena.byte_count() == 0, "the allocator's byte counter must return to zero once every page is released");
	Check(arena.page_count() == 0, "no pages must remain after every page is released");
}

void TestMissedPageFreeMutant() {
	LeakyPageArena leaky;
	leaky.add_page(4);
	leaky.add_page(8);
	leaky.add_page(16);
	void *last_page_ptr = leaky.pages.back().data; // captured before the buggy release drops it
	leaky.leaky_release_all();
	Check(leaky.bytes == 16 * sizeof(Sample),
	      "must-die: a release loop that stops one page short leaves exactly that page's bytes uncounted");
	::operator delete(last_page_ptr); // free for real, so this demonstration itself stays ASan-clean
}

// ---------------------------------------------------------------------
// dedup_policy's own row: "One test naming the policy and its consequence:
// the 50-vs-37.5 counter" — the real duplicate-timestamp counter bug this
// fence exists to catch: cerberus's test/property/gen/counter_dup.go
// dataset, pinned by its own TestPromQL_RateDupTimestamp_RowAndNative. A
// COUNTER sampled at (60,10),(120,20),(180,30),(240,40), with the last
// point duplicated in the raw ingest, over the (0, 300] window.
//
// The Prometheus extrapolation arithmetic itself (the 1.1x threshold, the
// half-interval fallback) belongs to Tier 4's `extrapolate` primitive,
// which #32 builds. `ToyExtrapolatedIncrease` below is a LOCAL, test-only
// reproduction of that published formula (not a re-derivation; the numbers
// are cerberus's), used only to show *why* the sample count sort_dedup
// corrects matters: it is the divisor of the average-interval estimate the
// 1.1x cap compares against.
// ---------------------------------------------------------------------
struct ToyExtrapolateInputs {
	double first_v, last_v;
	int64_t first_t, last_t;
	int64_t range_start, range_end;
	int sample_count;
};

double ToyExtrapolatedIncrease(const ToyExtrapolateInputs &in) {
	double sampled_interval = static_cast<double>(in.last_t - in.first_t);
	double avg_interval = sampled_interval / static_cast<double>(in.sample_count - 1);
	double cap = 1.1 * avg_interval;

	double duration_to_start = static_cast<double>(in.first_t - in.range_start);
	if (duration_to_start >= cap) {
		duration_to_start = avg_interval / 2.0;
	}
	double duration_to_end = static_cast<double>(in.range_end - in.last_t);
	if (duration_to_end >= cap) {
		duration_to_end = avg_interval / 2.0;
	}

	double value_range = in.last_v - in.first_v;
	double extrapolated_interval = duration_to_start + sampled_interval + duration_to_end;
	return value_range * extrapolated_interval / sampled_interval;
}

void TestDedupPolicyCounterConsequence() {
	SampleBuffer raw;
	raw.append(60, 10.0);
	raw.append(120, 20.0);
	raw.append(180, 30.0);
	raw.append(240, 40.0);
	raw.append(240, 40.0); // duplicate (Attributes, TimeUnix): the bug trigger
	Check(raw.size() == 5, "the raw ingest carries all 5 rows, duplicate included, before dedup");

	SampleBuffer deduped;
	deduped.merge(raw);
	deduped.sort_dedup();
	Check(deduped.size() == 4,
	      "sort_dedup must collapse the duplicate row, correcting the sample count the interval estimate divides by");

	ToyExtrapolateInputs correct {10.0, 40.0, 60, 240, 0, 300, static_cast<int>(deduped.size())};
	ToyExtrapolateInputs dup_inflated {10.0, 40.0, 60, 240, 0, 300, static_cast<int>(raw.size())};

	double correct_increase = ToyExtrapolatedIncrease(correct);
	double wrong_increase = ToyExtrapolatedIncrease(dup_inflated);

	char what[256];
	std::snprintf(what, sizeof(what), "the deduped sample count (4) must yield the Prometheus-correct increase 50, got %.6f",
	              correct_increase);
	Check(std::fabs(correct_increase - 50.0) < 1e-9, what);

	std::snprintf(what, sizeof(what),
	              "the dup-inflated sample count (5) must reproduce the documented wrong answer 37.5, got %.6f",
	              wrong_increase);
	Check(std::fabs(wrong_increase - 37.5) < 1e-9, what);
}

} // namespace

int main() {
	TestAppendGrowsAcrossPages();
	TestMergeEmptyNonemptyBoth();
	TestSortDedupOrderings();
	TestMergeThenSortDedupEqualsSortDedupOfConcatenation();
	TestStableSortOnlyTieBreakCanDetect();
	TestMutantDedupPolicyKeepsMin();
	TestSignedZeroTieBreak();
	TestNaNPayloadsTieBreak();
	TestStaleVsNaNTieBreak();
	TestSliceAtEveryBoundary();
	TestArenaDestructorReleasesEveryPage();
	TestMissedPageFreeMutant();
	TestDedupPolicyCounterConsequence();

	if (g_failures > 0) {
		std::fprintf(stderr, "sample_buffer_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("sample_buffer_test: PASS\n");
	return 0;
}
