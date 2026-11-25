// Host test of native/core/WeaponEnchant.h (the ch.8 gate's weapon enchantment rule). Needs no game and no CommonLib:
//   g++ -std=c++20 -Wall -Wextra -I native tools/weapon_enchant/weapon_enchant_test.cpp -o /path/t && /path/t
#include "core/WeaponEnchant.h"

#include <cstdio>

using apmf::weaponenchant::Hand;
using apmf::weaponenchant::IsOnHit;
using apmf::weaponenchant::NeedsEntry;

static int g_fail = 0;
#define CHECK(x) do { if (!(x)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

int main() {
    const Hand sword{ true, true, true, false, 0x1000, 0x1000 };
    CHECK(IsOnHit(sword));
    { Hand h = sword; h.isStaff = true; CHECK(!IsOnHit(h)); }            // a staff's enchantment is a cast
    { Hand h = sword; h.handCaster = false; CHECK(!IsOnHit(h)); }        // instant / voice caster: unchanged
    { Hand h = sword; h.isEnchant = false; CHECK(!IsOnHit(h)); }         // a spell is a cast whatever the hand holds
    { Hand h = sword; h.isWeapon = false; CHECK(!IsOnHit(h)); }          // empty hand, spell, torch, shield
    { Hand h = sword; h.weaponEnch = 0x2000; CHECK(!IsOnHit(h)); }       // another weapon's enchantment
    { Hand h = sword; h.weaponEnch = 0; CHECK(!IsOnHit(h)); }            // plain weapon
    { Hand h = sword; h.form = h.weaponEnch = 0; CHECK(!IsOnHit(h)); }   // a null never matches a null
    CHECK(!IsOnHit(Hand{}));
    // The entry is read only for a non-staff weapon asked about an enchantment, decided before the entry is known.
    Hand pre = sword; pre.weaponEnch = 0;
    CHECK(NeedsEntry(pre));
    { Hand h = pre; h.isStaff = true; CHECK(!NeedsEntry(h)); }           // a staff cast never touches the entry
    { Hand h = pre; h.isWeapon = false; CHECK(!NeedsEntry(h)); }
    { Hand h = pre; h.isEnchant = false; CHECK(!NeedsEntry(h)); }
    { Hand h = pre; h.handCaster = false; CHECK(!NeedsEntry(h)); }
    { Hand h = pre; h.form = 0; CHECK(!NeedsEntry(h)); }
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
