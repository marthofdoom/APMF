#pragma once
#include "APMF_API.h"

// ============================================================================
// APMF core -- the reusable ALLOWANCE TEMPLATE (Docs/ALLOWANCE-TEMPLATE.md §3).
// Shared infra every T2 attach point uses (and T1/T3/T4 will reuse):
//
//   * DerivesFrom      -- install-time RTTI derivation check (the ENGINE_NOTES
//                         §0.28 CombatMagicCasterArmor lesson: a vtable symbol
//                         that matches a name pattern is not proof it derives
//                         the expected class -- walk the RTTI to be sure).
//   * InstallOnVtables -- write_vfunc across a list of concrete vtable symbols,
//                         RTTI-verifying each first, storing per-vtable
//                         originals keyed by runtime address (the same
//                         recover-the-original / benign-if-foreign dispatch
//                         pattern MFO's CasterConsent.cpp/CombatStyle.cpp use).
//   * Allowed          -- the one shared "flip YES->NO" decision every T2
//                         instance makes once it has resolved (actor, intent,
//                         subject): a lock-free RCU ControlMap read
//                         (ControlMap::TryGetOwningClaim) -- NEVER a mutex,
//                         NEVER a follower-list touch (INVARIANTS #12/#13;
//                         contrast MFO's CombatStyle.cpp, which takes a mutex).
//
// The template's contract (§3): "decision vfunc -> resolve the deliberating
// actor -> lock-free claim lookup -> let the engine answer FIRST -> flip
// YES->NO only; never invent a YES, never call anything, no Tick, no
// re-assert." Every concrete T2 thunk (CastGate.cpp, EquipGate.cpp) follows
// this exact shape; only the resolver (how to get `actor` from `this`/args)
// and the subject (which FormID is being deliberated) differ per attach point.
// ============================================================================

namespace apmf::allowance {

    // Walk `vtableAddr`'s RTTI and confirm the class hierarchy includes
    // `expectedTypeDescriptor` -- either AS the class itself or one of its
    // bases. Standard MSVC x64 layout: the CompleteObjectLocator* sits at
    // vtableAddr-8, immediately before function-pointer slot 0 (the address a
    // `VTABLE_*[n]` symbol / `REL::Relocation<uintptr_t>::address()` itself
    // names); its ClassHierarchyDescriptor->baseClassArray always lists the
    // class itself at index 0, then every base (MSVC ABI). Returns false (do
    // NOT install) on a null COL, a missing hierarchy descriptor, or a
    // hierarchy that never names the expected type -- exactly the check that
    // would have caught VTABLE_CombatMagicCasterArmor (ENGINE_NOTES §0.28: a
    // vtable symbol with no real CombatMagicCaster-derived class behind it,
    // found only after a CTD; this walk catches it at install instead).
    bool DerivesFrom(std::uintptr_t vtableAddr, const void* expectedTypeDescriptor);

    // Install `thunk` at vtable slot `slot` on every symbol in `vtables`, after
    // confirming (DerivesFrom) each one's RTTI derives `expectedTypeDescriptor`
    // (pass nullptr to skip the check -- only for a symbol known to have NO
    // class at all; none in this codebase today). Stores the previous entry,
    // keyed by the vtable's runtime address, in `origMap` so the thunk can
    // recover the right original and treat any OTHER vtable as foreign
    // (benign default, never read its members). Returns the count actually
    // installed -- a non-deriving symbol is logged and skipped, never crashes
    // the install. Caller does the VR-refusal check (index verification is
    // SE/AE-only) and the install-once guard; this is the shared LOOP only.
    template <class ThunkFn>
    int InstallOnVtables(std::span<const REL::VariantID> vtables, std::size_t slot,
                         ThunkFn thunk, const void* expectedTypeDescriptor,
                         const char* tag,
                         std::unordered_map<std::uintptr_t, std::uintptr_t>& origMap) {
        int n = 0;
        for (const auto& id : vtables) {
            REL::Relocation<std::uintptr_t> vt{ id };
            if (expectedTypeDescriptor && !DerivesFrom(vt.address(), expectedTypeDescriptor)) {
                spdlog::warn("[{}] vtable 0x{:X} does NOT derive the expected RTTI base -- "
                             "SKIPPED (not installed; see ENGINE_NOTES §0.28).", tag, vt.address());
                continue;
            }
            origMap[vt.address()] = vt.write_vfunc(slot, thunk);
            ++n;
        }
        return n;
    }

    // The one shared "flip YES->NO" decision every T2 instance makes once it
    // has resolved (actor, intent, subject). ANY thread -- a lock-free RCU
    // ControlMap read (ControlMap::TryGetOwningClaim), never a mutex, never a
    // follower-list touch. Returns true (ALLOW -- let the engine's own answer
    // stand) when the actor is uncontrolled on this intent, the winning claim
    // carries no param (an all-zero APMF_Param means "channel default", never
    // a restriction), or the claim's param.form already matches `subjectForm`.
    // Returns false (DENY) only when a claim exists AND names a DIFFERENT
    // form -- APMF never invents a YES, only narrows one.
    bool Allowed(RE::FormID actor, APMF_API::Intent intent, RE::FormID subjectForm);

    // ch.8b (kIntent_Cast) allowance -- the cast-EXECUTION exclusivity, consulted by
    // CastGate (0x0A) and EquipGate (0x0F) AFTER the ch.8 SelectSpell check (BOTH
    // must pass). While an actor holds a winning kIntent_Cast claim, the ONLY spells
    // its AI may charge/re-arm are the claim's own spell + its runtime proxy -- so
    // the client's executed cast (which names exactly those) passes, and the AI's
    // competing choice for that hand fails. ANY thread -- a lock-free RCU
    // ControlMap read (ControlMap::TryGetCastClaim), never a mutex. Returns true
    // (ALLOW -- no cast claim, so ch.8b imposes nothing) when the actor has no
    // winning kIntent_Cast claim, or the claim names no spell/proxy (degenerate).
    // Returns false (DENY) only when a cast claim stands and `subjectForm` is
    // neither its spell nor its proxy.
    bool AllowedCast(RE::FormID actor, RE::FormID subjectForm);

}
