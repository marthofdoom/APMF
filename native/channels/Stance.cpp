#include "PCH.h"
#include "core/Log.h"
#include "core/Clock.h"
#include "core/MainThread.h"
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
// common case it stands as a clean one-shot. No per-tick re-assert. The ONE addition (2026-11-13): after Release the graph variable is
// confirmed and SneakStop re-sent (bounded, logged) until it takes, via mainthread::Post.
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
            m_pendingStop.erase(id);   // a fresh claim cancels any release confirm still running
            const bool wasSneaking = actor->IsSneaking();
            m_startedSneak[id] = !wasSneaking;   // did WE start it?
            bool sent = false, ok = false;
            if (!wasSneaking) { sent = true; ok = actor->NotifyAnimationGraph("SneakStart"); }
            spdlog::info("[ch.3] 0x{} sneak engaged (wasSneaking={}). Tier A promote. SneakStart sent={} accepted={} graph {}={} engine IsSneaking={}.",
                         apmf::log::Hex(id), wasSneaking, sent, ok, kSneakVar, GraphSneak(actor), actor->IsSneaking());
        }

        void Release(RE::FormID id, RE::Actor* actor) override {
            if (auto it = m_startedSneak.find(id); it != m_startedSneak.end()) {
                if (actor && it->second) {   // only undo what we started
                    const bool ok = actor->NotifyAnimationGraph("SneakStop");
                    spdlog::info("[ch.3] 0x{} sneak released. SneakStop accepted={} graph {}={} engine IsSneaking={}.",
                                 apmf::log::Hex(id), ok, kSneakVar, GraphSneak(actor), actor->IsSneaking());
                    // The bool is only "event queued"; the graph can ignore it (field 2026-11-13: released
                    // mid bow shot, actor stayed crouched while IsSneaking() was false). Confirm, and re-send
                    // until the graph variable drops. This COMPLETES the requested release; it does not mask.
                    if (GraphSneak(actor) > 0) {
                        m_pendingStop[id] = PendingStop{};
                        PostConfirm(id);
                    }
                } else {
                    spdlog::info("[ch.3] 0x{} sneak released.", apmf::log::Hex(id));
                }
                m_startedSneak.erase(it);   // drop per-NPC state keyed by id, even if actor is null
            }
        }

    private:
        static constexpr const char* kSneakVar = "IsSneaking";   // vanilla character 0_master.hkx variable (BSIsActiveModifier "IsSneaking_BSIsActiveModifier")
        static constexpr int         kMaxStopTries = 10;
        static constexpr std::uint64_t kStopGapMs  = 200;

        struct PendingStop { int tries = 0; std::uint64_t lastMs = 0; };

        // 1 = graph says sneaking, 0 = not, -1 = variable unreadable (then fall back to IsSneaking()).
        static int GraphSneak(RE::Actor* actor) {
            bool v = false;
            if (actor->GetGraphVariableBool(kSneakVar, v)) return v ? 1 : 0;
            return actor->IsSneaking() ? 1 : -1;
        }

        // One confirm hop per frame on the game thread (mainthread::Pump), re-Posting until the
        // graph is un-crouched, a new Engage supersedes it, or kMaxStopTries sends were made.
        void PostConfirm(RE::FormID id) {
            apmf::mainthread::Post([this, id]() {
                auto it = m_pendingStop.find(id);
                if (it == m_pendingStop.end()) return;   // superseded by Engage
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!actor || actor->IsDead()) { m_pendingStop.erase(it); return; }
                if (GraphSneak(actor) <= 0) {
                    spdlog::info("[ch.3] 0x{} release CONFIRMED (graph {}=false) after {} retry send(s).",
                                 apmf::log::Hex(id), kSneakVar, it->second.tries);
                    m_pendingStop.erase(it);
                    return;
                }
                const auto now = apmf::clock::MonotonicMs();
                if (now - it->second.lastMs >= kStopGapMs) {
                    if (it->second.tries >= kMaxStopTries) {
                        spdlog::warn("[ch.3] 0x{} release NOT confirmed: graph {} still true after {} SneakStop retries; giving up.",
                                     apmf::log::Hex(id), kSneakVar, it->second.tries);
                        m_pendingStop.erase(it);
                        return;
                    }
                    ++it->second.tries;
                    it->second.lastMs = now;
                    const bool ok = actor->NotifyAnimationGraph("SneakStop");
                    spdlog::warn("[ch.3] 0x{} still crouched after release: SneakStop retry {}/{} accepted={} engine IsSneaking={}.",
                                 apmf::log::Hex(id), it->second.tries, kMaxStopTries, ok, actor->IsSneaking());
                }
                PostConfirm(id);
            });
        }

        std::unordered_map<RE::FormID, bool> m_startedSneak;   // game-thread only
        std::unordered_map<RE::FormID, PendingStop> m_pendingStop;   // game-thread only (Release + Pump)
    };

}

APMF_REGISTER_CHANNEL(StanceChannel);
