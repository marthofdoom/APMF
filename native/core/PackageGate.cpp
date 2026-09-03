#include "PCH.h"
#include "core/Log.h"
#include "core/ControlMap.h"
#include "core/PackageGate.h"

// ============================================================================
// T3 -- CheckForCurrentAliasPackage (ch.9, Docs/CHANNEL-MAP.md). Graduated
// (2026-09-03) from the field-proven AliasPkgProbe (Docs/PROBE-ALLOWANCE.md
// "Probe 2" -- PROVEN for Phases 1-2, engage/release; Phase 3 save/load
// unexercised but the mechanism -- drop the claim without an engine call on
// kPreLoadGame -- is now the GENERIC ControlMap::ReleaseAll/Clear path every
// channel already gets for free, so this gate needs no bespoke Phase-3 code
// at all, unlike the probe which hand-rolled ClearOnPreLoad()).
//
// SAME hook (VTABLE_Character[0] slot 0x49 ONLY -- never PlayerCharacter, the
// §0.38 scar), SAME never-null contract (a claimed actor whose named package
// FormID doesn't resolve falls back to the engine's own answer, never a
// fabricated null -- §0.25 "claimed with nothing = rooted"), SAME
// EvaluatePackage(true,false) nudge (now called from channels/
// OfferPackage.cpp's Engage/OnOwnerChanged/Release, which already run on the
// game thread per Channel.h's contract -- exactly the thread AliasPkgProbe's
// OncePerFrame pump ran the identical call from).
//
// WHAT'S NEW vs the probe: the offered package is no longer one compile-time
// constant (`kProbePackageForm`) handed to every claimed actor -- it is
// APMF_Param::form on a REAL ControlMap claim (APMF_API::kIntent_OfferPackage),
// read via the lock-free RCU TryGetOwningClaim (Docs/ALLOWANCE-TEMPLATE.md
// §3, same discipline every T2 thunk already uses -- no mutex, no
// follower-list touch). The CLIENT decides which package to offer and owns
// that package's own runtime target (a targType-0 handle, or whatever the
// package itself resolves against); APMF only delivers whichever FormID the
// winning claim names.
// ============================================================================

namespace apmf::packagegate {

    namespace {

        std::atomic<bool> g_installed{ false };

        struct PkgHook {
            static RE::TESPackage* thunk(RE::Actor* a_this) {
                RE::TESPackage* orig = func(a_this);   // the engine's own answer first

                if (apmf::ControlMap::Get().ControlledCount() == 0) return orig;   // near-zero cost

                APMF_API::APMF_Param claim{};
                if (!apmf::ControlMap::Get().TryGetOwningClaim(a_this->GetFormID(),
                                                               APMF_API::kIntent_OfferPackage, claim))
                    return orig;   // uncontrolled on this intent
                if (claim.form == 0) return orig;   // claimed but no package named -- channel default, no redirect

                if (auto* pkg = RE::TESForm::LookupByID<RE::TESPackage>(claim.form)) return pkg;
                return orig;   // named FormID doesn't resolve -- degrade to the engine's own answer, never null
            }
            static inline REL::Relocation<decltype(thunk)> func;
            static constexpr std::size_t idx = 0x49;   // Actor::CheckForCurrentAliasPackage
        };

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[ch.9] VR runtime -- 0x49 index + EvaluatePackage reloc are SE/AE only; "
                         "package-offer allowance NOT installed.");
            return;
        }
        if (g_installed.exchange(true)) return;

        REL::Relocation<std::uintptr_t> charVtbl{ RE::VTABLE_Character[0] };
        PkgHook::func = charVtbl.write_vfunc(PkgHook::idx, PkgHook::thunk);

        spdlog::info("[ch.9] package-offer allowance hooked (Character::CheckForCurrentAliasPackage, 0x49) -- "
                     "a kIntent_OfferPackage claim with APMF_Param::form set to a TESPackage FormID redirects "
                     "that actor's alias-package answer to it; the engine runs it natively.");
    }

    void EvaluatePackage(RE::Actor* a_actor) {
        if (!a_actor) return;
        using func_t = void (*)(RE::Actor*, bool, bool);
        static REL::Relocation<func_t> func{ RELOCATION_ID(36407, 37401) };
        func(a_actor, true, false);   // resetAI MUST stay false -- never a full AI reset
    }

}
