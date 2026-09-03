#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 8 -- CASTING (SELECTION). Arbitration + claim lifecycle ONLY here;
// the real ENFORCEMENT lives one layer down, in the T2 allowance hooks
// (core/CastGate.cpp T2c CheckCast, core/EquipGate.cpp T2a CheckShouldEquip --
// Docs/ALLOWANCE-TEMPLATE.md §3/§7, Phase 2, marth 2026-09-02).
//
// APMF MODERATES; it does NOT generate behavior. SELECTING the spell is BEHAVIOR the
// CLIENT performs: MFO writes the follower's own `selectedSpells[slot]` and grants its
// own AI consent (MFO's CasterConsent) so the follower's combat AI DECIDES to cast the
// chosen spell itself -- a real, fully-animated cast, not a forced one. This channel
// itself still makes NO engine write (no `selectedSpells` write, no CastSpellImmediate)
// -- Engage/OnOwnerChanged/Release only log the claim lifecycle, exactly as before.
//
// What changed (Phase 2): the chosen spell riding in `param.form` is no longer
// observability-only. CastGate.cpp's CheckCast hook and EquipGate.cpp's
// CheckShouldEquip hook both read this SAME claim (ControlMap::TryGetOwningClaim,
// kIntent_SelectSpell) and deny any spell/staff that is NOT `param.form` -- so once a
// client claims this facet with a chosen spell, the engine's own AI can charge/equip
// ONLY that spell. APMF still never INVENTS a yes (CastGate/EquipGate only narrow the
// engine's own YES down to NO); the client's own consent grant (MFO's CasterConsent)
// is still what makes the AI WANT to cast in the first place -- this channel's claim
// only makes that choice EXCLUSIVE once the AI is willing. `Release` has nothing of
// its own to undo (the allowance hooks simply stop seeing a claim for this actor). No
// `Tick`.
// ============================================================================

namespace {

    class CastingSelectChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "casting-select"; }
        int              ChannelNo() const override { return 8; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_SelectSpell; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4B, "Numpad4 : CLAIM the casting facet (arbitration only)" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.8] 0x{} casting facet CLAIMED (chosen spell 0x{}). Arbitration only -- the "
                         "CLIENT selects the spell + grants its AI consent; APMF makes no cast write.",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.8] 0x{} casting claim RE-POINTED (chosen spell 0x{}).",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            spdlog::info("[ch.8] 0x{} casting facet released.", apmf::log::Hex(id));
        }
    };

}

APMF_REGISTER_CHANNEL(CastingSelectChannel);
