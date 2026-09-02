#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 8 -- CASTING (SELECTION), ARBITRATION-ONLY (moderator model, marth
// 2026-09-02).
//
// APMF MODERATES; it does NOT generate behavior. SELECTING the spell is BEHAVIOR the
// CLIENT performs: MFO writes the follower's own `selectedSpells[slot]` and grants its
// own AI consent (MFO's CasterConsent) so the follower's combat AI DECIDES to cast the
// chosen spell itself -- a real, fully-animated cast, not a forced one. APMF must not
// write `selectedSpells` or trigger a cast (no CastSpellImmediate). So this channel
// makes NO engine write.
//
// What it DOES: record that a client OWNS the casting facet, so APMF is the single
// arbiter (basis arbitration + claim lifecycle in the ControlMap). The chosen spell
// rides in `param.form` for observability and for a FUTURE suppression capability
// (deny a COMPETING framework's cast selection at the source -- a deep-hook gate, not
// built yet). NOTE: on the owned-cast path the client WANTS its AI to cast, so APMF
// must NOT suppress this actor's own casting here; a separate "suppress this actor's
// casting" capability (e.g. a combat-style magic-score deny) is a DIFFERENT facet and
// is never applied on the owned-cast path. `Release` has nothing to undo. No `Tick`.
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
