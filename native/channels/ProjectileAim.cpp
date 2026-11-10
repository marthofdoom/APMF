#include "PCH.h"
#include "core/ProjectileAim.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 29 -- PROJECTILE AIM (kIntent_ProjectileAim, ABI v36, card H5). ARBITRATION + claim lifecycle ONLY.
// The correction is enforced one layer down, at TESObjectWEAP::Fire's call to Projectile::Launch
// (core/ProjectileAim.cpp), which reads the WINNING claim through ControlMap::TryGetOwningClaimBasis and its
// budget from the claim's own param (ival). This channel makes no engine write: Engage / OnOwnerChanged /
// Release only log (and Engage re-checks that the seat is still in place).
// ============================================================================

namespace {

    class ProjectileAimChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "projectile-aim"; }
        int              ChannelNo() const override { return 29; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_ProjectileAim; }

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            const bool intact = apmf::projectileaim::SiteIntact();
            spdlog::info("[ch.29] 0x{} projectile aim ENGAGED: budget {} (0 = every shot corrected, 10+ = observe only){}.",
                         apmf::log::Hex(id), param.ival,
                         intact ? "" : " -- WARNING: the seat's call site was re-patched by another DLL");
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.29] 0x{} projectile aim: a different claim now owns the channel (budget {}).",
                         apmf::log::Hex(id), param.ival);
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            spdlog::info("[ch.29] 0x{} projectile aim released: every shot leaves as the engine launches it.",
                         apmf::log::Hex(id));
        }
        // No Tick: the correction lives entirely in the launch seat (INVARIANTS #1).
    };

}

APMF_REGISTER_CHANNEL(ProjectileAimChannel);
