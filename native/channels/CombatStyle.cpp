#include "PCH.h"
#include "core/CombatStyle.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 33 -- COMBAT STYLE (kIntent_CombatStyle, ABI v45, MFO T2 batch M item 5 "strict class style").
// ARBITRATION + claim lifecycle ONLY. The effect lives one layer down, at the engine's two reads of an actor's
// combat style (core/CombatStyle.cpp: two call-site seats), which read the slot table this channel fills.
// This channel makes no engine write: Engage / OnOwnerChanged / Release keep that table, and core/CombatStyle's
// Heartbeat asks the ENGINE to refresh the controller's cached style (its own UpdateCombatStyle) afterwards.
// ============================================================================

namespace {

    class CombatStyleChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "combat-style"; }
        int              ChannelNo() const override { return 33; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_CombatStyle; }

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            apmf::combatstyle::SetClaim(id, actor, param, "ENGAGED");
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            apmf::combatstyle::SetClaim(id, actor, param, "RE-POINTED (the winning claim's style applies)");
        }

        // Relinquish (INVARIANTS #5a): nothing of Harbinger's to restore, because nothing was written. The seats
        // stop answering at once; the controller's cached style is re-derived by the engine's own refresh, asked
        // for on the next frame.
        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            apmf::combatstyle::ClearClaim(id);
            spdlog::info("[ch.33] 0x{} combat style released: the actor reads the engine's own combat style again.",
                         apmf::log::Hex(id));
        }
        // No Tick: the effect lives entirely in the seats (INVARIANTS #1).
    };

}

APMF_REGISTER_CHANNEL(CombatStyleChannel);
