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
//
// TWO INDEPENDENTLY-SELECTABLE GROUPS (marth 2026-09-05): a deck run of this
// probe produced a NEW crash signature that did not occur before the probe
// was deployed. The probe is the prime suspect but NOT proven (the crash
// shares mid-chain frames with pre-existing, unrelated Targeting-hook cast
// CTDs). To let the next run isolate which seat (if either) is implicated,
// install is split into two groups, each gated by its OWN flag in
// Data/SKSE/Plugins/APMF.ini, [AiCastSeats] section, both 0/OFF by default:
//   GROUP A -- EnableItemScoreProbe=1  -- CalculateScore only (item vtables)
//   GROUP B -- EnableCasterSeatProbe=1 -- CheckStartCast/CheckStopCast/
//                                         GetMagicTarget (caster vtables)
// A deck run sets exactly ONE of the two to 1 to isolate it. No hotkey/toggle
// (standing rule: probes are config-gated or always-on rate-limited logging
// only, never a runtime input switch) -- see AiCastSeats.cpp's ReadIniFlag.
//
// FALLBACK SAFETY: every thunk's original-recovery is now non-fabricating --
// see AiCastSeats.cpp's file banner (RecoverLiveOriginal) for how a
// structurally-unreachable lookup miss is handled without ever inventing a
// score/decision/target the engine did not actually produce.
//
// ROOT CAUSE, FOUND (marth 2026-09-05): the deck CTD above WAS this probe --
// CommonLib's `GetMagicTarget` declaration (2 args, `void*` return) is WRONG.
// The real engine ABI has a HIDDEN 16-byte out-slot (Microsoft x64 sret
// convention for an aggregate return that doesn't fit a register); the
// original thunk, written to CommonLib's wrong shape, shifted every real
// argument one register and handed back garbage read out of unrelated
// process memory. Fixed in AiCastSeats.cpp (see its STANDING RULE banner and
// the corrected `Out16`/3-arg `GetMagicTarget_t`). CalculateScore/
// CheckStartCast/CheckStopCast were re-checked and are NOT susceptible (all
// three return a scalar that can never trigger the sret convention).
//
// STANDING RULE: a CommonLib vfunc declaration is not ABI-trustworthy on its
// own -- verify the disassembled callee before writing a thunk, especially
// for anything typed `void*`/`unk`, and especially for a possible hidden-
// return out-slot.
//
// GROUP C (marth 2026-09-06, Opus PASS S brief): a THIRD, independent probe in
// this same file -- CalculateScore (0x0C) on the four WEAPON-class
// CombatInventoryItem leaves (Melee, Ranged, Shield, Torch), which have NO
// CommonLib concrete class/vtable symbol at all (Docs/DENY-COMPLETENESS-
// AUDIT.md row 15's documented gap -- the reason `core/EquipGate.cpp` cannot
// hook 0x0F for weapons). Resolved from raw disasm-confirmed RVAs
// (`REL::Offset`, no `REL::VariantID` -- no Address-Library ID exists for any
// of the four), AE-only, gated by an install-time function-pointer-at-slot
// identity check (no confirmed RTTI name exists for these four, so that check
// stands in for the name-match guard the other seats use). Ships ENABLED by
// default (`[AiCastSeats] EnableWeaponScoreProbe`, default 1) -- CalculateScore
// is scalar-return, categorically immune to the GetMagicTarget-class sret bug
// documented above. A SEPARATE flag (`[AiCastSeats] EnableScoreSteer`, default
// 0) adds a fixed upward bias to a claimed form's own returned score (reading
// the same ch.15 `kIntent_Equipment` claim `core/EquipGate.cpp`'s T2a gate
// already reads) -- stays OFF until this probe's own field data confirms 0x0C
// actually runs for a follower. Melee+Ranged share arbitration category 0,
// Shield+Torch share category 3 -- a weapon score can bias which OF THOSE
// wins, never beat a spell that already claimed the hand (weapons are walked
// after the spell/staff categories in the engine's fixed order table). See
// AiCastSeats.cpp's GROUP C block comment (above `WeaponScoreThunk`) for the
// full design.
// ============================================================================

namespace apmf::aicastseats {

    // Install whichever of the three observe-hook groups its own INI flag
    // enables (once). Call at kDataLoaded. VR-refused (the vtable indices
    // below are SE/AE-only verified, matching every other T2-shaped seat in
    // this codebase; GROUP C is additionally AE-only, refused on SE 1.5.97).
    // Idempotent.
    void Install();

}
