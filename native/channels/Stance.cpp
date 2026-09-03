#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 3 -- STANCE / SNEAK. SANCTIONED BOUNDED ONE-SHOT PROMOTE (INVARIANTS
// #0(c), Tier A, CHANNEL-MAP ch.3): drive the crouch via
// NotifyAnimationGraph("SneakStart"/"SneakStop") (IAnimationGraphManagerHolder
// vfunc 01, bound). A stance toggle has no meaningful deny form and no AI decision
// to arbitrate around, so a single deterministic call at Engage/Release is lawful
// under #0(c), not a stand-in awaiting conversion. We record whether SHE was already
// sneaking and only toggle the difference, restoring on release. Note (ch.3
// caveat): the anim-event drive is ADDITIVE over the package's own crouch decision,
// so on a package that actively forces stance this is not a hard block -- for the
// common case it stands as a clean one-shot. No per-tick re-assert.
// ============================================================================

namespace {

    class StanceChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "stance-sneak"; }
        int              ChannelNo() const override { return 3; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_Stance; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x49, "Numpad9 : toggle sneak/crouch" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& /*param*/) override {
            if (!actor) return;
            const bool wasSneaking = actor->IsSneaking();
            m_startedSneak[id] = !wasSneaking;   // did WE start it?
            if (!wasSneaking) actor->NotifyAnimationGraph("SneakStart");
            spdlog::info("[ch.3] 0x{} sneak engaged (wasSneaking={}). Tier A promote.", apmf::log::Hex(id), wasSneaking);
        }

        void Release(RE::FormID id, RE::Actor* actor) override {
            if (auto it = m_startedSneak.find(id); it != m_startedSneak.end()) {
                if (actor && it->second) actor->NotifyAnimationGraph("SneakStop");   // only undo what we started
                spdlog::info("[ch.3] 0x{} sneak released.", apmf::log::Hex(id));
                m_startedSneak.erase(it);   // drop per-NPC state keyed by id, even if actor is null
            }
        }

    private:
        std::unordered_map<RE::FormID, bool> m_startedSneak;   // game-thread only
    };

}

APMF_REGISTER_CHANNEL(StanceChannel);
