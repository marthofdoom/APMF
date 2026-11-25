// Host test of native/core/RowEquipLogic.h (the ch.8b row equip admission) and native/core/LogDedupe.h.
//   g++ -std=c++20 -O2 -Wall -Wextra -I native tools/row_equip/row_equip_test.cpp -o /path/t && /path/t
#include "core/LogDedupe.h"
#include "core/RowEquipLogic.h"

#include <cstdio>
#include <initializer_list>

using namespace apmf::rowequip;
using namespace APMF_API;
static int g_fail = 0, g_n = 0;
#define CHECK(x) do { ++g_n; if (!(x)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

int main() {
    for (const std::uint32_t row : { kCastSeat_Ward, kCastSeat_BoundItem, kCastSeat_Stagger, kCastSeat_Paralyze }) {
        CHECK(Decide(row, false, false, false, false, false) == Keep::kAdmit);
        CHECK(Decide(row, true, false, false, false, false) == Keep::kFleeing);
        CHECK(Decide(row, false, true, false, false, false) == Keep::kApplied);
        CHECK(Decide(row, false, false, false, false, true) == Keep::kBroke);
    }
    CHECK(Decide(kCastSeat_BoundItem, false, false, true, false, false) == Keep::kApplied);   // on the caster himself
    CHECK(Decide(kCastSeat_Stagger, false, false, true, false, false) == Keep::kAdmit);
    CHECK(Decide(kCastSeat_Ward, false, false, false, true, false) == Keep::kWardFloor);
    CHECK(Decide(kCastSeat_BoundItem, false, false, false, true, false) == Keep::kAdmit);     // a ward's rule only
    CHECK(WardTooLow(0.5f, 0.5f) && WardTooLow(0.2f, 0.5f) && !WardTooLow(0.51f, 0.5f));      // the engine wants MORE than it
    CHECK(WardTooLow(MagickaShare(50, 100), 0.5f) && !WardTooLow(MagickaShare(90, 100), 0.5f));
    CHECK(MagickaShare(0, 0) == 1.0f);
    CHECK(OwnRow(kCastSeat_Ward) && OwnRow(kCastSeat_BoundItem) && !OwnRow(kCastSeat_Summon) && !OwnRow(0));

    APMF_Param a{}, b{};
    a.ival = b.ival = 5;
    a.fval = b.fval = 1.5f;
    CHECK(apmf::logdedupe::SameParam(a, b));
    for (int i = 0; i < 7; ++i) {
        b = a;
        switch (i) {
        case 0: b.form = 1; break;
        case 1: b.fval = 2; break;
        case 2: b.ival = 6; break;
        case 3: b.target = 1; break;
        case 4: b.posX = 1; break;
        case 5: b.posY = 1; break;
        default: b.posZ = 1; break;
        }
        CHECK(!apmf::logdedupe::SameParam(a, b));
    }
    std::printf(g_fail ? "row_equip: %d FAILED of %d\n" : "row_equip: PASS (%d failed of %d)\n", g_fail, g_n);
    return g_fail != 0;
}
