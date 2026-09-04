// Mirrors registry-static-assert-red.cpp with the corrected determinism: the
// same SLICE+SUM_ABS combination is legal once det is D1 — this is the
// registry's own real `sum_over_time` shape (docs/design/surface.md, RANGE
// family). A red-only proof isn't real proof that IsValidRow's rule fires on
// the (state, scale_kind) pair specifically rather than always failing, so
// this fixture must compile cleanly (exit 0) where the red one must not.
#include "../../src/kernel/registry_types.hpp"

static_assert(chronoduck::IsValidRow(chronoduck::Family::RANGE, chronoduck::StateClass::SLICE,
                                      chronoduck::Determinism::D1, chronoduck::ScaleKind::SUM_ABS),
              "registry-static-assert-green: SLICE+D1+SUM_ABS must be accepted by IsValidRow");

int main() {
  return 0;
}
