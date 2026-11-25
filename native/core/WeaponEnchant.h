#pragma once

#include <cstdint>

// ============================================================================
// Is the form a HAND caster's CheckCast (0x0A) asks about the ENCHANTMENT of the
// weapon in that hand? Then it is a weapon hit applying its enchantment, not a
// cast, and no cast allow-list governs it.
//
// The engine's on-hit road, read on 1.6.1170 / 1.5.97 / 1.7.104
// (0x6C2FF0 / 0x6310A0 / 0x6D5AB0): enchantment = InventoryEntryData::GetEnchantment
// of the hand's equipped entry, caster = GetMagicCaster(leftHand ? 0 : 1), and the
// cast launch asks that caster's CheckCast with the enchantment as the form. A STAFF
// charges its enchantment through the same slot as a real cast, so it stays governed.
//
// PURE LOGIC, std only: tools/weapon_enchant/weapon_enchant_test.cpp runs this code.
// core/CastGate.cpp reads the inputs.
// ============================================================================
namespace apmf::weaponenchant {

    struct Hand {
        bool          handCaster   = false;     // the caster is the left or right hand caster
        bool          isEnchant    = false;     // the form is an EnchantmentItem (formType 21)
        bool          isWeapon     = false;     // that hand's equipped entry holds a TESObjectWEAP
        bool          isStaff      = false;     // ...of weapon type staff
        std::uintptr_t form        = 0;         // the form asked about
        std::uintptr_t weaponEnch  = 0;         // the enchantment the ENGINE reads off that entry (0 = none)
    };

    constexpr bool IsOnHit(const Hand& a_h) {
        return a_h.handCaster && a_h.isEnchant && a_h.isWeapon && !a_h.isStaff && a_h.form != 0 &&
               a_h.form == a_h.weaponEnch;
    }

}
