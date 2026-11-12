#include "PCH.h"
#include "core/Aversion.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 32 -- AVERSION (kIntent_Aversion, ABI v41, MFO class plan card H2 "less likely to be targeted").
// ARBITRATION + claim lifecycle ONLY. The effect lives one layer down, at the standard target selector's scorer
// (core/Aversion.cpp: three call-site seats), which reads the WINNING claim through
// ControlMap::TryGetOwningClaimBasis and the claimant's reference handle from the table this channel fills.
// This channel makes no engine write: Engage / OnOwnerChanged / Release keep that table and log.
// ============================================================================

namespace {

    class AversionChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "aversion"; }
        int              ChannelNo() const override { return 32; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_Aversion; }

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            apmf::aversion::SetClaimant(id, actor, param, "ENGAGED");
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            apmf::aversion::SetClaimant(id, actor, param, "RE-POINTED (the winning claim's weight applies)");
        }

        // Relinquish (INVARIANTS #5a): nothing to restore. The engine's own selection is unshaped from its next
        // scoring pass on; a foe that was averted keeps its current target until the engine re-scores (1 to 3 s).
        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            apmf::aversion::ClearClaimant(id);
            spdlog::info("[ch.32] 0x{} aversion released: foes select this actor by the engine's own score again.",
                         apmf::log::Hex(id));
        }
        // No Tick: the effect lives entirely in the seats (INVARIANTS #1).
    };

}

APMF_REGISTER_CHANNEL(AversionChannel);
