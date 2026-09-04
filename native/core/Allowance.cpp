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
        RE::FormID spell = 0, proxy = 0;
        if (!ControlMap::Get().TryGetCastClaim(actor, spell, proxy))
            return true;                                   // no cast claim -> ch.8b imposes nothing
        if (spell == 0 && proxy == 0) return true;         // degenerate/no spell named -> allow
        if (subjectForm == spell || subjectForm == proxy) return true;
        return false;                                      // a cast claim stands and this is neither -> DENY
    }

}
