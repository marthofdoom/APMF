#pragma once

// ============================================================================
// PASSIVE, OBSERVE-ONLY deck probe -- confirms four engine control seats a
// disassembly pass (1.6.1170 + Address Library; MFO/APMF scratchpad notebook
// "fable-cast-re-findings.md") concluded drive the combat AI's OWN cast
// decision, BEFORE any architecture is built on the claim. This installs NO
// deny, NO force, NO argument/return-value change of any kind -- it only
// watches four vfunc calls the AI already makes and logs what it sees, then
// chains to the original unconditionally.
//
// THE FOUR SEATS (marth's table):
//   WHICH item   -- CombatInventoryItem::CalculateScore   (vfunc 0x0C)
//   WHETHER cast -- CombatMagicCaster::CheckStartCast     (vfunc 0x06)
//   WHERE it aims-- CombatMagicCaster::GetMagicTarget     (vfunc 0x0A)
//   HOW LONG     -- CombatMagicCaster::CheckStopCast      (vfunc 0x07)
//
// RTTI-VERIFIED INSTALLS ONLY -- no blind vtable slot, no invented Address
// Library ID. Every vtable list below is a set ALREADY proven live and
// version-resilient elsewhere in this codebase, hooked at a DIFFERENT slot
// than the existing user (write_vfunc on a distinct index never disturbs the
// existing hook on the same symbol):
//   * CalculateScore -- the same 30 concrete
//     CombatInventoryItemMagicT<CombatInventoryItemMagic|CombatInventoryItemStaff,
//     CombatMagicCaster*> vtables core/EquipGate.cpp already RTTI-verifies and
//     patches at slot 0x0F (CheckShouldEquip). RTTI base: RE::RTTI_CombatInventoryItem.
//   * CheckStartCast / CheckStopCast / GetMagicTarget -- the same 14 concrete
//     CombatMagicCaster vtables MFO's native/CasterConsent.cpp already
//     RTTI-verifies (informally; this probe adds the formal
//     allowance::DerivesFrom walk) and patches at slot 0x06. VTABLE_CombatMagicCasterArmor
//     is DELIBERATELY EXCLUDED -- ENGINE_NOTES §0.28 / CasterConsent.cpp's own
//     comment: a vtable symbol with no real class behind it. RTTI base:
//     RE::RTTI_CombatMagicCaster.
//
// See AiCastSeats.cpp for the exact log-line shape and throttling (all four
// seats are per-actor+per-subject rate-limited to ~1.5s, matching the
// existing probes' cadence discipline).
// ============================================================================

namespace apmf::aicastseats {

    // Install all four observe hooks (once). Call at kDataLoaded. VR-refused
    // (the vtable indices below are SE/AE-only verified, matching every other
    // T2-shaped seat in this codebase). Idempotent.
    void Install();

}
