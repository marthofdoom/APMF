#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"
#include "core/PackageGate.h"

// ============================================================================
// Channel 9 -- PACKAGE-PROCEDURE ACTIVITY. Arbitration + claim lifecycle +
// the EvaluatePackage nudge live here; the real ENFORCEMENT (what
// CheckForCurrentAliasPackage actually RETURNS) lives in core/PackageGate.cpp
// (T3, graduated 2026-09-03 from the field-proven AliasPkgProbe,
// Docs/PROBE-ALLOWANCE.md "Probe 2" -- PROVEN for Phases 1-2).
//
// A client claims this facet naming the TESPackage FormID to offer
// (APMF_Param::form). PackageGate.cpp's 0x49 hook then returns that package
// for the claim's actor instead of the framework's own answer -- the engine
// adopts it and runs it NATIVELY (real pathing/procedures), one
// OnPackageChange each way. The CLIENT owns the package's own runtime target
// (a targType-0 handle, or whatever the package itself resolves against);
// APMF only delivers whichever FormID the winning claim names -- it never
// picks, validates, or interprets the package's own content.
//
// One EvaluatePackage(true,false) nudge on Engage/OnOwnerChanged (so the
// redirect takes within one eval instead of waiting for the engine's own
// natural poll) and on Release (so the framework package resumes
// immediately) -- exactly the AliasPkgProbe.cpp mechanism, now event-driven
// off the Channel lifecycle (already game-thread-only per Channel.h/
// ControlMap.h's contract) instead of a queued hotkey op.
// ============================================================================

namespace {

    class OfferPackageChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "offer-package"; }
        int              ChannelNo() const override { return 9; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_OfferPackage; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0xB5, "NumpadSlash : CLAIM the package-offer facet (arbitration only; the test surface "
                        "carries no package, so a test claim offers nothing -- see APMF_RequestEx)" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.9] 0x{} package-offer facet CLAIMED (package 0x{}). core/PackageGate.cpp's 0x49 "
                         "hook is what actually redirects the actor's package answer.",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
            apmf::packagegate::EvaluatePackage(actor);
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.9] 0x{} package-offer claim RE-POINTED (package 0x{}).",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
            apmf::packagegate::EvaluatePackage(actor);
        }

        void Release(RE::FormID id, RE::Actor* actor) override {
            spdlog::info("[ch.9] 0x{} package-offer facet released -- framework package resumes.",
                         apmf::log::Hex(id));
            apmf::packagegate::EvaluatePackage(actor);
        }
    };

}

APMF_REGISTER_CHANNEL(OfferPackageChannel);
