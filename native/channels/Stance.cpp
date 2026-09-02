#include "PCH.h"
#include "core/Registry.h"

// ============================================================================
// Channel 3 -- STANCE / SNEAK. Tier A promote (CHANNEL-MAP ch.3): drive the crouch
// via NotifyAnimationGraph("SneakStart"/"SneakStop") (IAnimationGraphManagerHolder
// vfunc 01, bound). We record whether SHE was already sneaking and only toggle the
// difference, restoring on release. Note (ch.3 caveat): the anim-event drive is
// ADDITIVE over the package's own crouch decision, so on a package that actively
// forces stance this is not a hard block -- for the common case it stands as a
// clean one-shot. No per-tick re-assert.
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

        void Engage(RE::Actor* actor) override {
            if (!actor) return;
            const bool wasSneaking = actor->IsSneaking();
            m_startedSneak[actor->GetFormID()] = !wasSneaking;   // did WE start it?
            if (!wasSneaking) actor->NotifyAnimationGraph("SneakStart");
            spdlog::info("[ch.3] 0x{:08X} sneak engaged (wasSneaking={}). Tier A promote.",
                         actor->GetFormID(), wasSneaking);
        }

        void Release(RE::Actor* actor) override {
            if (actor) {
                if (auto it = m_startedSneak.find(actor->GetFormID()); it != m_startedSneak.end()) {
                    if (it->second) actor->NotifyAnimationGraph("SneakStop");   // only undo what we started
                    spdlog::info("[ch.3] 0x{:08X} sneak released.", actor->GetFormID());
                }
                m_startedSneak.erase(actor->GetFormID());
            }
        }

    private:
        std::unordered_map<RE::FormID, bool> m_startedSneak;   // game-thread only
    };

}

APMF_REGISTER_CHANNEL(StanceChannel);
