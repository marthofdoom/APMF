#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 14 -- SHOUT / POWER SELECTION, ARBITRATION-ONLY (moderator model, marth
// 2026-09-02).
//
// APMF MODERATES; it does NOT generate behavior. SELECTING which shout occupies the
// voice slot is BEHAVIOR the CLIENT performs (a decision-selection input the AI later
// triggers on its own) -- the same shape as ch.8 casting selection. This channel
// previously called `ActorEquipManager::EquipShout` directly, which was the same
// anti-pattern that got ch.6/ch.8 fixed after the StartCombat CTD (INVARIANTS #0):
// APMF selecting-what-the-AI-decides instead of the client. No crash risk (EquipShout
// is bound and safe), but it duplicated the mistake -- so this channel makes NO
// engine equip write.
//
// What it DOES: record that a client OWNS the shout/power facet, so APMF is the
// single arbiter (basis arbitration + claim lifecycle in the ControlMap). The chosen
// shout rides in `param.form` for observability and for a FUTURE suppression
// capability (deny a competing framework's own `EquipShout` write at the source -- a
// deep-hook gate, not built yet). The CLIENT selects the shout via its own
// `ActorEquipManager::EquipShout` call; APMF makes no equip write. `Release` has
// nothing to undo. No `Tick`.
// ============================================================================

namespace {

    class ShoutPowerChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "shout-power"; }
        int              ChannelNo() const override { return 14; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_ShoutPower; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x37, "NumpadStar : CLAIM the shout/power facet (arbitration only)" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.14] 0x{} shout/power facet CLAIMED (chosen shout 0x{}). Arbitration only -- "
                         "the CLIENT selects the shout via its own EquipShout; APMF makes no equip write.",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.14] 0x{} shout/power claim RE-POINTED (chosen shout 0x{}).",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
        }

        void Release(RE::FormID id, RE::Actor*) override {
            spdlog::info("[ch.14] 0x{} shout/power facet released.", apmf::log::Hex(id));
        }
    };

}

APMF_REGISTER_CHANNEL(ShoutPowerChannel);
