#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 6 -- COMBAT-TARGET, ARBITRATION-ONLY (moderator model, marth 2026-09-02).
//
// APMF MODERATES; it does NOT generate behavior. Commanding/holding a combat target
// is BEHAVIOR, and behavior belongs to the CLIENT: MFO steers its follower's combat
// target with its OWN proven mechanism (a compare-and-write of the AIProcess
// `currentCombatTarget`) -- APMF must not do it. So this channel makes NO engine combat
// call at all (no StartCombat -- that also caused a hard AV from a bad reloc signature
// -- and no `currentCombatTarget` write).
//
// What it DOES: record that a client OWNS the combat-target facet, so APMF is the
// single arbiter of who controls it (basis arbitration + claim lifecycle in the
// ControlMap). The intended target rides in `param.form` for observability and for a
// FUTURE suppression capability (deny a competing framework's target writes at the
// source -- a deep-hook gate, not built yet). Until that exists, ownership is recorded
// and nothing is executed. `Release` relinquishes -- there is nothing to undo, and a
// steer facet must never "undo" a live engine decision (INVARIANTS #5a). No `Tick`
// (a clean arbitration record does no per-frame work, #1).
// ============================================================================

namespace {

    class CombatTargetChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "combat-target"; }
        int              ChannelNo() const override { return 6; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_CombatTarget; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4A, "NumpadMinus : CLAIM the combat-target facet (arbitration only)" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.6] 0x{} combat-target facet CLAIMED (intended target 0x{}). Arbitration "
                         "only -- the CLIENT commands the target; APMF makes no combat call.",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.6] 0x{} combat-target claim RE-POINTED (intended target 0x{}).",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            spdlog::info("[ch.6] 0x{} combat-target facet released (relinquished).", apmf::log::Hex(id));
        }
    };

}

APMF_REGISTER_CHANNEL(CombatTargetChannel);
