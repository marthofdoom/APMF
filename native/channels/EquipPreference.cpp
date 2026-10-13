#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 26 -- EQUIP PREFERENCE (kIntent_EquipPreference, ABI v22, 2026-10-12, weighting
// phase 1). ARBITRATION + claim lifecycle ONLY. The preference is enforced one layer down, at the
// CalculateScore (0x0C) seats Harbinger already owns (core/AiCastSeats.cpp -> core/EquipPreference.cpp
// OnScore, which reads the WINNING claim through ControlMap::TryGetOwningClaimBasis and its
// declaration from equippref's own snapshot), and -- only under kEquipPref_Force -- at the equip
// sink (core/EquipSink.cpp). This channel makes no engine write: Engage / OnOwnerChanged / Release
// only log, so an escalation to force is visible in APMF.log the moment it applies (D4: every
// escalation is loud). The declaration of a released claim is dropped by ClientAPI's Release.
// ============================================================================

namespace {

    bool Forced(const APMF_API::APMF_Param& a_param) {
        return (static_cast<std::uint32_t>(a_param.ival) & APMF_API::kEquipPref_Force) != 0;
    }

    void LogFlags(const char* a_what, RE::FormID a_id, const APMF_API::APMF_Param& a_param) {
        const auto bits    = static_cast<std::uint32_t>(a_param.ival);
        const auto unknown = bits & ~static_cast<std::uint32_t>(APMF_API::kEquipPref_Force);
        if (Forced(a_param))
            spdlog::warn("[ch.26] 0x{} equip preference {} with kEquipPref_Force: ESCALATED -- the equip sink now "
                         "refuses non-preferred engine equips into the preferred hands, ranked by the Equip facet "
                         "priority (a source that outranks the claim keeps its equip: a FORCE ANOMALY).{}",
                         apmf::log::Hex(a_id), a_what,
                         unknown ? fmt::format(" Unknown flag bits 0x{} ignored.", apmf::log::Hex(unknown)) : "");
        else
            spdlog::info("[ch.26] 0x{} equip preference {} (score lift only, no force).{}", apmf::log::Hex(a_id), a_what,
                         unknown ? fmt::format(" Unknown flag bits 0x{} ignored.", apmf::log::Hex(unknown)) : "");
    }

    class EquipPreferenceChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "equip-preference"; }
        int              ChannelNo() const override { return 26; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_EquipPreference; }

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            LogFlags("ENGAGED", id, param);
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            LogFlags("now", id, param);
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            spdlog::info("[ch.26] 0x{} equip preference released: its items score as the engine scores them.",
                         apmf::log::Hex(id));
        }
        // No Tick: the lift lives entirely in the score seats (INVARIANTS #1).
    };

}

APMF_REGISTER_CHANNEL(EquipPreferenceChannel);
