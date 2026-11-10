#include "PCH.h"
#include "core/PotionAllowance.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 30 -- POTION ALLOWANCE (kIntent_PotionAllowance, ABI v37, ClickUp 1dfcuabcqpf). ARBITRATION + claim
// lifecycle ONLY. The refusals are enforced one layer down, at the potion items' slot 0x0F and Character slot
// 0x10F (core/PotionAllowance.cpp), which read the WINNING claim through ControlMap::TryGetOwningClaimBasis and
// its predicate from the claim's own declaration (APMF_API_v37::SetPotionAllowance). This channel makes no
// engine write: Engage / OnOwnerChanged / Release only log.
// ============================================================================

namespace {

    class PotionAllowanceChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "potion-allowance"; }
        int              ChannelNo() const override { return 30; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_PotionAllowance; }

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& /*param*/) override {
            spdlog::info("[ch.30] 0x{} potion allowance ENGAGED: the claim's declared potions are refused at the AI's "
                         "selection and at every drink (SetPotionAllowance; a claim with no declaration refuses nothing).",
                         apmf::log::Hex(id));
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& /*param*/) override {
            spdlog::info("[ch.30] 0x{} potion allowance: a different claim now owns the channel (its declaration applies).",
                         apmf::log::Hex(id));
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            spdlog::info("[ch.30] 0x{} potion allowance released: the actor drinks what the engine chooses again.",
                         apmf::log::Hex(id));
        }
        // No Tick: the refusals live entirely in the two seats (INVARIANTS #1).
    };

}

APMF_REGISTER_CHANNEL(PotionAllowanceChannel);
