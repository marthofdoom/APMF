#include "PCH.h"
#include "core/Registry.h"
#include "core/Arbiter.h"

// ============================================================================
// Channel 5 -- HEADTRACKING. Own the per-type headtrack slot (design.md Section
// 4b Tier A, CHANNEL-MAP ch.5). Test facet: crane the head straight up (a point
// above the actor).
//
// DOCUMENTED FALLBACK (re-assert): the AI co-writes the very slot we own, so a
// clean source-gate is impossible for this facet -- there is no separate input to
// deny, only the shared output slot. We therefore re-assert each tick. This is
// the design's flagged exception (Section 1a rule 3): owning a slot the AI also
// writes, NOT forcing a computed downstream output. It is the ONLY channel here
// that overrides Tick(). See Docs/INVARIANTS.md #2.
// ============================================================================

namespace {

    constexpr float kLookUpHeight = 300.0f;

    void AssertLookUp(RE::Actor* a) {
        auto* proc = a->GetActorRuntimeData().currentProcess;
        if (!proc) return;
        RE::NiPoint3 up = a->GetPosition();
        up.z += kLookUpHeight;                  // directly overhead -> look straight up
        proc->SetHeadtrackTarget(a, up);
    }

    class HeadtrackChannel final : public apmf::Channel {
    public:
        const char* Name() const override { return "headtrack"; }
        int         ChannelNo() const override { return 5; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x51, "Numpad3 : toggle look-straight-up (own the headtrack slot)" },
            };
            return keys;
        }

        void OnHotkey(std::uint32_t, RE::Actor* target) override {
            if (engaged.load()) { Release(target ? target : apmf::Arbiter::Get().CurrentTarget()); return; }
            if (!target) { spdlog::warn("[ch.5] REFUSED -- no gated target."); return; }
            engaged.store(true);
            AssertLookUp(target);
            spdlog::info("[ch.5] engaged on 0x{:08X} -- owning the headtrack slot (look up). Re-asserted each "
                         "tick (documented fallback: AI co-writes this slot).", target->GetFormID());
        }

        void Tick(RE::Actor* actor) override { AssertLookUp(actor); }

        void Release(RE::Actor*) override {
            if (!engaged.exchange(false)) return;
            // Stop asserting; the AI resumes ownership of the slot on its next tick.
            spdlog::info("[ch.5] released -- stop owning the headtrack slot; AI resumes pointing the head.");
        }
    };

}

APMF_REGISTER_CHANNEL(HeadtrackChannel);
