// kahan.hpp — kahan_add / kahan_merge, a Tier 0 numeric primitive
// (`docs/design/primitives.md:tier0-row:` `(Neumaier, FMA-defeating, Inf-safe)`).
// This is the compensated-summation state every `D1`-class SLICE sum partial
// carries (`docs/design/architecture.md:d1-sums:` `Neumaier-compensated addition`).
// Deliberately dependency-free: no `#include "duckdb.hpp"`, compiles
// standalone with a bare `g++`/`clang++ -std=c++17` — the pattern
// `comparator.hpp` established for Article V.1's TU-per-primitive rule.
//
// Algorithm: Neumaier's improved Kahan-Babuska summation, the same shape as
// the reference's own compensated summation
// (`docs/testing/corpora.md:kahansum:` `Neumaier-variant compensated sum`) —
// a magnitude-tested correction term, one running compensation, carried
// forward across calls.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace chronoduck {

// One running compensated sum: `sum` is the running total, `comp` the
// accumulated rounding error Neumaier's correction recovers. The true value
// is `sum + comp`; a caller finalizes by adding the two once, at the end —
// this header never collapses them itself, so a partial can keep
// accumulating indefinitely without losing the correction.
struct KahanState {
	double sum = 0.0;
	double comp = 0.0;
};

// Adds one term to a running compensated sum.
//
// FMA-defeating (`docs/design/primitives.md`'s own parenthetical for this
// primitive — see the header citation above; the reference's own summation
// util uses the same defence,
// `docs/testing/corpora.md:kahansum:` `FMA-defeating casts`): Neumaier's
// correction is only exact when every intermediate below rounds to true
// `double` precision immediately, never carried in a wider register or
// folded across statements by the compiler — the class of hazard a fused
// multiply-add or x87-style excess precision both represent. `#pragma STDC
// FP_CONTRACT OFF` is silently ignored by this codebase's compiler (no
// multiply appears in this formula for it to fuse in any case), so the
// portable defence used here is the classic one for compensated summation in
// C++: `volatile`-qualified intermediates force an actual round-and-store
// after each step, which is what a "cast" accomplishes in a language whose
// arithmetic already rounds every store.
//
// Inf-safe (`docs/design/primitives.md`'s other named guarantee — see the
// header citation — and the must-die mutant
// `docs/testing/primitives.md:kahan-mutant-inf:` `removing the Inf special case`):
// once either operand or the raw sum is infinite, Neumaier's correction term
// below is a subtraction of two possibly-infinite quantities and can
// manufacture a NaN even where the true sum is well-defined
// (`Inf + finite == Inf`) — so this bypasses the correction entirely the
// moment `std::isinf` fires on any of the three, letting IEEE-754's own
// Inf/NaN arithmetic decide `sum` directly.
inline KahanState kahan_add(KahanState state, double x) {
	const double old_sum = state.sum;
	volatile double t = old_sum + x;
	if (std::isinf(t) || std::isinf(old_sum) || std::isinf(x)) {
		state.sum = t;
		return state;
	}
	// The magnitude test (the other must-die mutant,
	// `docs/testing/primitives.md:kahan-mutant-magnitude:` `swapping the magnitude test`):
	// whichever operand has the larger magnitude anchors the subtraction, so
	// the correction below recovers the *exact* rounding error of
	// `old_sum + x` rather than an approximation of it — Neumaier's whole
	// point over plain Kahan summation, which only handles the
	// `|old_sum| >= |x|` case.
	if (std::fabs(old_sum) >= std::fabs(x)) {
		volatile double correction = (old_sum - t) + x;
		state.comp += correction;
	} else {
		volatile double correction = (x - t) + old_sum;
		state.comp += correction;
	}
	state.sum = t;
	return state;
}

namespace detail {

inline uint64_t KahanBits(double v) {
	uint64_t bits;
	std::memcpy(&bits, &v, sizeof(bits));
	return bits;
}

// A total, deterministic order over two `KahanState`s that is a pure
// function of their *values*, never of which one the caller happened to
// pass as the first argument to `kahan_merge` below. This is what lets
// `kahan_merge` satisfy the invariant
// `docs/testing/primitives.md:kahan-invariant-commute:` `merge(a,b) == merge(b,a)`
// bit-exact even when `a.sum` and `b.sum` tie in magnitude — the raw bit
// pattern breaks the tie deterministically, still without reference to
// argument position.
inline bool KahanOutranks(const KahanState &x, const KahanState &y) {
	const double ax = std::fabs(x.sum), ay = std::fabs(y.sum);
	if (ax != ay)
		return ax > ay;
	const double acx = std::fabs(x.comp), acy = std::fabs(y.comp);
	if (acx != acy)
		return acx > acy;
	const uint64_t sx = KahanBits(x.sum), sy = KahanBits(y.sum);
	if (sx != sy)
		return sx > sy;
	return KahanBits(x.comp) >= KahanBits(y.comp);
}

} // namespace detail

// Merges two independently-accumulated compensated sums into one, order-free
// (the invariant cited above, plus
// `docs/testing/primitives.md:kahan-invariant-identity:` `merge of a partial with the zero state is identity`).
// Canonicalizes the pair by `detail::KahanOutranks` before folding, so the
// physical computation performed is a pure function of the *unordered pair*
// `{a, b}` — never of which one arrived as the first argument — which is
// what a `D1` SLICE partial's `combine` needs: the operator's
// `docs/design/architecture.md:where-it-plugs-in:` `Aggregate functions`
// hand `combine` partials from arbitrary, non-contiguous morsels, in
// whatever order the scheduler produced them.
inline KahanState kahan_merge(KahanState a, KahanState b) {
	if (!detail::KahanOutranks(a, b)) {
		std::swap(a, b);
	}
	KahanState result = kahan_add(a, b.sum);
	result = kahan_add(result, b.comp);
	return result;
}

} // namespace chronoduck
