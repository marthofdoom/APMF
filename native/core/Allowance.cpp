#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/ControlMap.h"

// ============================================================================
// See core/Allowance.h for the design. This file holds the two non-template
// pieces: the RTTI derivation walk (DerivesFrom) and the shared allow/deny
// read (Allowed). InstallOnVtables stays header-only (a template).
// ============================================================================

namespace apmf::allowance {

    bool DerivesFrom(std::uintptr_t vtableAddr, const void* expectedTypeDescriptor) {
        if (!vtableAddr || !expectedTypeDescriptor) return false;

        // The CompleteObjectLocator* immediately precedes function-pointer slot
        // 0 in the standard MSVC x64 vtable layout -- a raw dereference of
        // already-mapped module memory, not a relocation (vtableAddr is already
        // the resolved runtime address).
        auto* colPtr = *reinterpret_cast<RE::RTTI::CompleteObjectLocator**>(
            vtableAddr - sizeof(void*));
        if (!colPtr) return false;

        // ClassHierarchyDescriptor::classDescriptor is a single RVA directly TO
        // the struct -- RVA<T>::get() alone is correct here.
        auto* hierarchy = colPtr->classDescriptor.get();
        if (!hierarchy) return false;

        // ClassHierarchyDescriptor::baseClassArray is NOT a direct array of
        // BaseClassDescriptor -- per the real MSVC x64 RTTI layout
        // (_RTTIClassHierarchyDescriptor::pBaseClassArray -> _RTTIBaseClassArray,
        // itself `DWORD arrayOfBaseClassDescriptors[numBaseClasses]`), it is ONE
        // MORE level of indirection: an RVA to an array of numBaseClasses RVAs,
        // each of which THEN points to one BaseClassDescriptor. CommonLib's
        // `RVA<BaseClassDescriptor>::get()` alone resolves only the outer RVA, so
        // walk the inner DWORD array by hand rather than treating its target as a
        // BaseClassDescriptor[] directly (that would read the array as if each
        // entry were 0x18 bytes instead of the real 4-byte RVA stride --
        // exactly the kind of silent-garbage bug this whole check exists to
        // avoid, ENGINE_NOTES §0.28).
        const std::uint32_t arrayRva = hierarchy->baseClassArray.offset();
        if (arrayRva == 0) return false;
        auto* rvaArray = REL::Relocation<std::uint32_t*>{ REL::Offset(arrayRva) }.get();
        if (!rvaArray) return false;

        for (std::uint32_t i = 0; i < hierarchy->numBaseClasses; ++i) {
            const std::uint32_t descRva = rvaArray[i];
            if (descRva == 0) continue;
            auto* desc = REL::Relocation<RE::RTTI::BaseClassDescriptor*>{ REL::Offset(descRva) }.get();
            if (!desc) continue;
            // BaseClassDescriptor::typeDescriptor IS a direct single RVA to the
            // type_info struct -- RVA<T>::get() is correct here.
            const auto* td = desc->typeDescriptor.get();
            if (static_cast<const void*>(td) == expectedTypeDescriptor) return true;
        }
        return false;
    }

    bool Allowed(RE::FormID actor, APMF_API::Intent intent, RE::FormID subjectForm) {
        APMF_API::APMF_Param claim{};
        // ch.8 SetSpellAllowList (APMF_API_v4): a winning claim may carry a bounded
        // ADDITIONAL allow-set beyond claim.form (e.g. MFO's castLvl exempt
        // heal/buff set riding a live offense-gambit claim,
        // Docs/SPEC-GRADUATED-CAST.md). Fixed local buffer, no heap traffic on
        // this hot combat-thread path. A claim that never calls SetSpellAllowList
        // has allowCount == 0 and every read below is byte-identical to before
        // this widening existed.
        RE::FormID    allowSet[APMF_API::kMaxSpellAllowList];
        std::uint32_t allowCount = 0;
        if (!ControlMap::Get().TryGetOwningClaim(actor, intent, claim, allowSet, allowCount))
            return true;                                                              // uncontrolled -> allow
        if (claim.form == 0 && allowCount == 0) return true;                          // no param, no allow-set -> channel default
        if (claim.form == subjectForm) return true;
        for (std::uint32_t i = 0; i < allowCount; ++i) {
            if (allowSet[i] == subjectForm) return true;
        }
        return false;
    }

    bool AllowedCast(RE::FormID actor, RE::FormID subjectForm) {
        RE::FormID    spell = 0, proxy = 0;
        std::uint32_t flags = 0;
        if (!ControlMap::Get().TryGetCastClaim(actor, spell, proxy, &flags))
            return true;                                   // no cast claim -> ch.8b imposes nothing
        // kCastFlag_DenyHandOnly: the winning claim drives NOTHING and exists only
        // to close its hand. Its spell/proxy are 0 by construction (ApplyRequest
        // forces them), so the "degenerate -> allow" line below would otherwise read
        // a deliberate DENY as an accident and allow everything -- the exact hole
        // the flag was added to close. Checked FIRST, and only on the flag, so a
        // genuinely degenerate claim still degrades as it always did.
        if (flags & APMF_API::kCastFlag_DenyHandOnly) return false;   // hand claimed purely to deny -> DENY
        if (spell == 0 && proxy == 0) return true;         // degenerate/no spell named -> allow
        if (subjectForm == spell || subjectForm == proxy) return true;
        return false;                                      // a cast claim stands and this is neither -> DENY
    }

    namespace {
        // feat/per-hand-cast-claims: apmf::allowance::Hand -> apmf::CastHand. Two
        // separate enums on purpose (Allowance.h stays free of a ControlMap.h
        // include-dependency) -- this is the one place they meet.
        apmf::CastHand ToCastHand(Hand h) {
            switch (h) {
                case Hand::kLeft:  return apmf::CastHand::kLeft;
                case Hand::kRight: return apmf::CastHand::kRight;
                default:           return apmf::CastHand::kUnknown;
            }
        }
    }

    bool AllowedCastForHand(RE::FormID actor, RE::FormID subjectForm, Hand callerHand) {
        // feat/per-hand-cast-claims: reads the claim that OCCUPIES `callerHand`
        // (kCastFlag_DualCast occupies both; `kUnknown` forwards to the unscoped,
        // actor-wide floor -- byte-identical to the pre-existing behavior for a
        // caller that cannot resolve a hand). This used to read the single
        // actor-wide best-basis claim and then compare its OWN hand field against
        // `callerHand` by hand -- with two concurrent claims now possible, that
        // comparison could pick the OTHER hand's claim and wrongly treat THIS
        // hand as unclaimed (or vice versa); ControlMap::TryGetCastClaimForHand
        // does the correct per-hand arbitration once, here.
        RE::FormID    spell = 0, proxy = 0;
        std::uint32_t flags = 0;
        if (!ControlMap::Get().TryGetCastClaimForHand(actor, ToCastHand(callerHand), spell, proxy, &flags))
            return true;                                   // no cast claim on THIS hand -> ch.8b imposes nothing
        // kCastFlag_DenyHandOnly (2026-09-06): the claim that OCCUPIES this hand was
        // taken purely to CLOSE it -- the client drives nothing here and declared
        // that nothing else may either. Its spell/proxy are 0 by construction
        // (core/ControlMap.cpp ApplyRequest forces them), so without this line the
        // "degenerate -> allow" case below would read that deliberate silence as an
        // accident and leave the hand wide open, which is precisely the RC3 hole
        // (25 Stone Runes on the unclaimed hand) the flag exists to close. The deny
        // is the SAME per-hand deny a driving claim already imposes -- one path, not
        // a second mechanism -- and it stops at this hand: the other hand, and an
        // actor with no claim at all, are untouched.
        if (flags & APMF_API::kCastFlag_DenyHandOnly) return false;   // hand claimed purely to deny -> DENY
        if (spell == 0 && proxy == 0) return true;         // degenerate/no spell named -> allow
        if (subjectForm == spell || subjectForm == proxy) return true;
        return false;                                      // a cast claim stands on THIS hand and this is neither -> DENY
    }

    bool CastClaimNamesForHand(RE::FormID actor, RE::FormID subjectForm, Hand callerHand) {
        // See Allowance.h for WHY this exists (H1: APMF denied its own delivery-flip
        // proxy). Deliberately NOT expressed in terms of AllowedCastForHand: THAT
        // returns true for "no claim at all", which may not license an override
        // of the ch.8 narrow. This is the strict POSITIVE form -- a claim must
        // actually stand, on this hand, naming this exact form. Same per-hand
        // read as AllowedCastForHand above (feat/per-hand-cast-claims).
        RE::FormID    spell = 0, proxy = 0;
        std::uint32_t flags = 0;
        if (!ControlMap::Get().TryGetCastClaimForHand(actor, ToCastHand(callerHand), spell, proxy, &flags))
            return false;                                  // no cast claim on THIS hand -> nothing to admit
        // A deny-only claim NAMES NOTHING -- it exists to close the hand, not to
        // admit anything into it. Its spell/proxy are already 0 so the next line
        // would return false anyway; stated explicitly because this is the strict
        // POSITIVE predicate and "a claim stands" must never be mistaken here for
        // "a claim admits something".
        if (flags & APMF_API::kCastFlag_DenyHandOnly) return false;
        if (spell == 0 && proxy == 0) return false;        // degenerate -> names nothing
        return subjectForm != 0 && (subjectForm == spell || subjectForm == proxy);
    }

}
