// Mirrors comparator-headroom-accept-red.cpp/comparator-headroom-reject-red.cpp
// with the real, current kReorderFactor: both margins clear the floors
// docs/testing/comparator.md states ("three to four orders" accept-side,
// "ten orders" reject-side) today, so this fixture must compile cleanly
// (exit 0) — a red-only proof isn't real proof that ReorderFactorHeadroom's
// check fires on genuine erosion specifically, rather than always failing.
#include "../../src/kernel/comparator.hpp"

static_assert(chronoduck::ReorderFactorHeadroom(chronoduck::kReorderFactor, chronoduck::kUnitRoundoff).accept_orders >=
                  1e3,
              "comparator-headroom-green: the real kReorderFactor must clear the accept-side floor");
static_assert(chronoduck::ReorderFactorHeadroom(chronoduck::kReorderFactor, chronoduck::kUnitRoundoff).reject_orders >=
                  1e10,
              "comparator-headroom-green: the real kReorderFactor must clear the reject-side floor");

int main() {
	return 0;
}
