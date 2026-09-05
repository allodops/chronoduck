// Proves ReorderFactorHeadroom's reject-side check is mechanically enforced:
// a hypothetical reorder factor 100x below the smallest-rejected-divergence
// edge (docs/testing/comparator.md's duplicate-timestamp bug at 3e-2) has
// only two orders of headroom left, not the "ten orders" the document
// states — so the static_assert below must fail to compile. Proven by
// scripts/hygiene-selftest.py, which compiles this file directly with a
// bare g++ and asserts on the nonzero exit code AND the static_assert
// message appearing in stderr, never exit code alone.
#include "../../src/kernel/comparator.hpp"

static_assert(chronoduck::ReorderFactorHeadroom(3e-2 / 100.0, chronoduck::kUnitRoundoff).reject_orders >= 1e10,
              "comparator-headroom-reject-red: a reorder factor eroded toward the divergence edge must fail the floor");

int main() {
	return 0;
}
