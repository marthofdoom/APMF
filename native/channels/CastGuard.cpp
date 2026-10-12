#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 25 -- CAST GUARD (kIntent_CastGuard, ABI v21, 2026-10-12). ARBITRATION + claim
// lifecycle ONLY. The guards are enforced one layer down, at the cast seats Harbinger already
// owns: core/CastGate.cpp (0x0A CheckCast on the hand casters) and core/CastSeats.cpp (0x06
// CheckStartCast), both reading the winning live claim through ControlMap::TryGetCastGuard;
// the line-of-fire measurement is core/CastGuard.cpp's main-thread Pump. This channel makes
// no engine write: Engage / OnOwnerChanged / Release only log, so a claim's bits are visible
// in APMF.log the moment they apply. The claim is bounded (param.fval seconds, the cast TTL
// rules, ControlMap ApplyRequest) and Repoint renews it.
// ============================================================================

namespace {

    std::string Bits(std::int32_t a_ival) {
        const auto b = static_cast<std::uint32_t>(a_ival);
        std::string s;
        if (b & APMF_API::kCastGuard_LineOfFire) s += "line-of-fire ";
        if (b & APMF_API::kCastGuard_ChannelRight) s += "channel-bound-R ";
        if (b & APMF_API::kCastGuard_ChannelLeft) s += "channel-bound-L ";
        if (s.empty()) s = "none ";
        s.pop_back();
        return s;
    }

    class CastGuardChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "cast-guard"; }
        int              ChannelNo() const override { return 25; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_CastGuard; }

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.25] 0x{} cast guard ENGAGED ({}) -- enforced at CheckCast 0x0A (hand casters) and "
                         "CheckStartCast 0x06; Harbinger makes no engine write.",
                         apmf::log::Hex(id), Bits(param.ival));
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.25] 0x{} cast guard now ({}).", apmf::log::Hex(id), Bits(param.ival));
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            spdlog::info("[ch.25] 0x{} cast guard released.", apmf::log::Hex(id));
        }
        // No Tick: the guards live entirely in the seat consults (INVARIANTS #1).
    };

}

APMF_REGISTER_CHANNEL(CastGuardChannel);
