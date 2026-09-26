#include "PCH.h"
#include "core/ActionGate.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 23 -- the in-combat PURSUIT LEASH (kIntent_PursuitLeash, ABI v16,
// ClickUp 86e3ex5ve batch L). Arbitration + claim lifecycle ONLY here. The
// ENFORCEMENT is core/ActionGate.cpp: the same act()+pop() ForceFail pair ch.7
// uses, plus the update() seat (vtable slot 0x04), on the ten pursuit leaves and
// the four search leaves. The seats consult THIS intent's winning claim,
// separately from ch.7's category winner, so a leash and another mod's ch.7
// offense/cast deny never replace each other.
//
// The claim names the ANCHOR actor (param.target) and the RADIUS (param.fval).
// Engage / OnOwnerChanged hand the winner's pair to ActionGate's leash table on
// the game thread (SetLeash resolves the anchor to a HANDLE there -- the seats
// never look up a form, the ch.20 precedent); Release clears it. A save load or
// new game drops the table (plugin.cpp ResetLeash). This channel makes no engine
// write of its own.
// ============================================================================

namespace {

    class PursuitLeashChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "pursuit-leash"; }
        int              ChannelNo() const override { return 23; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_PursuitLeash; }

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.23] 0x{} pursuit-leash facet CLAIMED (anchor 0x{}, radius {:.0f}).",
                         apmf::log::Hex(id), apmf::log::Hex(param.target), param.fval);
            apmf::actiongate::SetLeash(id, param);
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.23] 0x{} pursuit-leash claim RE-POINTED (anchor 0x{}, radius {:.0f}).",
                         apmf::log::Hex(id), apmf::log::Hex(param.target), param.fval);
            apmf::actiongate::SetLeash(id, param);
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            spdlog::info("[ch.23] 0x{} pursuit-leash facet released.", apmf::log::Hex(id));
            apmf::actiongate::ClearLeash(id);
        }
    };

}

APMF_REGISTER_CHANNEL(PursuitLeashChannel);
