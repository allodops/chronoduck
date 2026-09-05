// stale_test.cpp — the L1a direct test for `src/kernel/stale.hpp`
// (docs/testing/layers.md's L1a row: "every Tier 0-5 primitive has its own
// translation unit, its own table-driven tests ... exercised directly").
// Hand-rolled `main()`, no test framework, compiled and run with a bare
// `g++ -std=c++17` by `scripts/hygiene/kernel-primitive-tests.py`.
//
// Follows `docs/testing/primitives.md`'s Tier 0 `is_stale / stale_marker`
// row exactly: bit-pattern round-trips, the one invariant, the reference's
// pinned bit pattern as the oracle, and the one must-die mutant.
#include "kernel/stale.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using chronoduck::is_stale;
using chronoduck::kStaleNaNBits;
using chronoduck::stale_marker;

namespace {

int g_failures = 0;

void Check(bool condition, const char *what) {
	if (!condition) {
		std::fprintf(stderr, "stale_test: FAIL — %s\n", what);
		g_failures++;
	}
}

uint64_t Bits(double v) {
	uint64_t bits;
	std::memcpy(&bits, &v, sizeof(bits));
	return bits;
}

// The independent oracle: `docs/testing/primitives.md`'s "The reference's
// StaleNaN bit pattern, as a constant fixture" — Prometheus's `value.go`
// literal, reconstructed here from its own documented bits rather than by
// calling `stale_marker()`, so this test doesn't validate the primitive
// against itself.
constexpr uint64_t kReferenceStaleNaNBits = 0x7ff0000000000002ULL;

// Unit contract: "Bit pattern round-trips".
void TestBitPatternRoundTrip() {
	Check(kStaleNaNBits == kReferenceStaleNaNBits,
	      "kStaleNaNBits must match the reference's documented StaleNaN bit pattern exactly");
	Check(Bits(stale_marker()) == kReferenceStaleNaNBits,
	      "stale_marker() must reconstruct the reference's exact bit pattern");
	// Round-trip through is_stale in both directions: the marker classifies
	// as stale, and a value built from any other bit pattern does not.
	Check(is_stale(stale_marker()), "is_stale(stale_marker()) must be true");
}

// Unit contract: "ordinary NaN is not stale; stale is NaN".
void TestOrdinaryNaNIsNotStale() {
	Check(std::isnan(stale_marker()), "stale_marker() must be a NaN");
	double ordinary_nan = 0.0 / 0.0;
	Check(std::isnan(ordinary_nan), "sanity: 0.0/0.0 must actually be a NaN on this platform");
	Check(Bits(ordinary_nan) != kStaleNaNBits,
	      "sanity: the ordinary NaN this platform produces must not itself be the stale bit pattern");
	Check(!is_stale(ordinary_nan), "an ordinary NaN must not be classified as stale");
}

// The invariant `docs/testing/primitives.md` states directly:
// `is_stale(stale_marker()) ∧ isnan(stale_marker()) ∧ ¬is_stale(0.0/0.0)`.
void TestPrimitivesMdInvariant() {
	Check(is_stale(stale_marker()) && std::isnan(stale_marker()) && !is_stale(0.0 / 0.0),
	      "is_stale(stale_marker()) and isnan(stale_marker()) and not is_stale(0.0/0.0), the primitive's stated "
	      "invariant, must all hold together");
}

// Not stale: a real finite value, positive and negative infinity, and every
// other NaN payload the test can construct by hand.
void TestNonStaleValues() {
	Check(!is_stale(0.0), "0.0 must not be stale");
	Check(!is_stale(1.5), "a finite value must not be stale");
	Check(!is_stale(-1.5), "a finite negative value must not be stale");
	Check(!is_stale(std::numeric_limits<double>::infinity()), "+Inf must not be stale");
	Check(!is_stale(-std::numeric_limits<double>::infinity()), "-Inf must not be stale");

	// Other NaN payloads, built by hand from bit patterns adjacent to the
	// real one — the must-die mutant this guards against ("Payload
	// comparison widened to any NaN") would classify every one of these as
	// stale too.
	auto FromBits = [](uint64_t bits) {
		double v;
		std::memcpy(&v, &bits, sizeof(v));
		return v;
	};
	double quiet_nan = FromBits(0x7ff8000000000000ULL);
	double payload_off_by_one_low = FromBits(kStaleNaNBits + 1);
	double payload_off_by_one_high = FromBits(kStaleNaNBits - 1);
	double negative_stale_bit_pattern = FromBits(kStaleNaNBits | (1ULL << 63));

	Check(std::isnan(quiet_nan) && !is_stale(quiet_nan), "a different quiet NaN payload must not be stale");
	Check(std::isnan(payload_off_by_one_low) && !is_stale(payload_off_by_one_low),
	      "a NaN payload one bit above the stale marker must not be stale");
	Check(std::isnan(payload_off_by_one_high) && !is_stale(payload_off_by_one_high),
	      "a NaN payload one bit below the stale marker must not be stale");
	Check(std::isnan(negative_stale_bit_pattern) && !is_stale(negative_stale_bit_pattern),
	      "must-die (payload comparison widened to any NaN): the stale marker's payload with the sign bit set is a "
	      "*different* bit pattern and must not be classified as stale by a comparison that only checked the payload");
}

} // namespace

int main() {
	TestBitPatternRoundTrip();
	TestOrdinaryNaNIsNotStale();
	TestPrimitivesMdInvariant();
	TestNonStaleValues();

	if (g_failures > 0) {
		std::fprintf(stderr, "stale_test: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	std::printf("stale_test: PASS\n");
	return 0;
}
