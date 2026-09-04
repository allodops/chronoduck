// stale.hpp — is_stale / stale_marker, a Tier 0 numeric primitive
// (`docs/design/primitives.md:tier0-row-is-stale:` `is_stale`).
// The reference's staleness NaN payload, preserved bit-for-bit so `NULL`
// always means "no row" and an ordinary NaN and the stale marker are told
// apart by this primitive
// (`docs/design/architecture.md:staleness-payload:` `the two NaNs are told apart by`).
// Deliberately dependency-free: no `#include "duckdb.hpp"`, compiles
// standalone with a bare `g++`/`clang++ -std=c++17` — the pattern
// `comparator.hpp` and `kahan.hpp` established for Article V.1's
// TU-per-primitive rule.
//
// `src/kernel/comparator.hpp` used to carry a private copy of this bit
// pattern (its own header comment named this file as where the shared
// primitive belongs); it now includes this header and calls `is_stale`
// through its own `is_stale_nan` name instead, so the bit pattern has
// exactly one definition.
#pragma once

#include <cstdint>
#include <cstring>

namespace chronoduck {

// The reference's staleness NaN, bit-for-bit: Prometheus's `value.go`
// `StaleNaN`, `math.Float64frombits(0x7ff0000000000002)` — the primitive's
// own independent oracle is this exact constant, pinned by a fixture
// (`docs/testing/primitives.md:stale-oracle:` `as a constant fixture`).
constexpr uint64_t kStaleNaNBits = 0x7ff0000000000002ULL;

inline uint64_t StaleDoubleToBits(double v) {
	uint64_t bits;
	std::memcpy(&bits, &v, sizeof(bits));
	return bits;
}

// The stale marker itself: a specific NaN payload, not merely "some NaN".
// `stale_marker()` always reconstructs it from the pinned bit pattern above,
// rather than from a `double` literal, since NaN payloads are not guaranteed
// to survive a floating-point literal or arithmetic expression unchanged.
inline double stale_marker() {
	double v;
	uint64_t bits = kStaleNaNBits;
	std::memcpy(&v, &bits, sizeof(v));
	return v;
}

// Bit-pattern equality against the pinned stale marker — never "is this any
// NaN", which is the must-die mutant `docs/testing/primitives.md` names for
// this primitive ("Payload comparison widened to any NaN"). An ordinary NaN
// (e.g. `0.0 / 0.0`) is a legitimate non-stale answer and must not be
// classified as stale.
inline bool is_stale(double v) {
	return StaleDoubleToBits(v) == kStaleNaNBits;
}

} // namespace chronoduck
