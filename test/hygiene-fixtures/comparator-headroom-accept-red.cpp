// Proves ReorderFactorHeadroom's accept-side check is mechanically enforced:
// a hypothetical reorder factor sitting exactly at the largest-accepted-
// drift edge (5 * kUnitRoundoff, docs/testing/comparator.md's "1-5 ULP")
// has zero headroom left, not the "three to four orders" the document
// states — so the static_assert below must fail to compile. Proven by
// scripts/hygiene-selftest.py, which compiles this file directly with a
// bare g++ and asserts on the nonzero exit code AND the static_assert
// message appearing in stderr, never exit code alone.
#include "../../src/kernel/comparator.hpp"

static_assert(
    chronoduck::ReorderFactorHeadroom(5.0 * chronoduck::kUnitRoundoff, chronoduck::kUnitRoundoff).accept_orders >= 1e3,
    "comparator-headroom-accept-red: a reorder factor eroded to the accept edge must fail the floor");

int main() {
	return 0;
}
