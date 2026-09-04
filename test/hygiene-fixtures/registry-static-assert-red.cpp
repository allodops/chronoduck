// Fixture proving IsValidRow's static rule is mechanically enforced: a
// SLICE state with a float-sum scale_kind (SUM_ABS) must be D1, never D0
// (docs/design/surface.md's "any state whose partial contains a float sum
// is D1 unless it uses reproducible_sum" rule). This combination is
// invalid, so the static_assert below must fail to compile — proven by
// scripts/hygiene-selftest.mjs, which compiles this file directly with a
// bare g++ and asserts on the nonzero exit code AND the static_assert
// message appearing in stderr, never exit code alone.
#include "../../src/kernel/registry_types.hpp"

static_assert(chronoduck::IsValidRow(chronoduck::Family::RANGE, chronoduck::StateClass::SLICE,
                                     chronoduck::Determinism::D0, chronoduck::ScaleKind::SUM_ABS),
              "registry-static-assert-red: SLICE+D0+SUM_ABS must be rejected by IsValidRow");
