#include "PCH.h"
#include "core/Registry.h"

// ============================================================================
// Channel 5 -- HEADTRACKING. Test facet: crane the head straight up.
//
// **KNOWN-INCOMPLETE BLOCK** (deck-tested 2026-09-02; INVARIANTS #2). APMF's job is
// to BE THE GATE -- block the foreign input so nothing competes. This channel does
// NOT yet do that: the AI writes MULTIPLE headtrack TYPES (default/combat/dialogue/
// procedure) and we only own the one point-based slot, so we do not block the AI's
// own writes. On a package-locked follower the package reclaims the head via a
// higher-priority type -- a visible per-tick war. The Tick() re-assert below is the
// SYMPTOM of a missing block, a flagged STOPGAP, never a clean gate.
//
// The REAL fix (not built): block the AI's headtrack write for this channel AT THE
// 0xAD hook (skip/neutralize the AI's per-type write when APMF owns the channel).
// Until then this stays a known-incomplete block that can lose to an aggressive
// source. It is the ONE channel that overrides Tick().
// ============================================================================

namespace {

    constexpr float kLookUpHeight = 300.0f;

    void AssertLookUp(RE::Actor* a) {
        if (!a) return;
        auto* proc = a->GetActorRuntimeData().currentProcess;
        if (!proc) return;
        RE::NiPoint3 up = a->GetPosition();
        up.z += kLookUpHeight;              // directly overhead -> look straight up
        proc->SetHeadtrackTarget(a, up);    // owns the point-based slot; not all types
    }

    class HeadtrackChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "headtrack"; }
        int              ChannelNo() const override { return 5; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_Headtrack; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x51, "Numpad3 : toggle look-straight-up (KNOWN-INCOMPLETE block)" },
            };
            return keys;
        }

        void Engage(RE::Actor* actor) override {
            AssertLookUp(actor);
            spdlog::info("[ch.5] 0x{:08X} look-up -- KNOWN-INCOMPLETE block; Tick re-asserts (stopgap for the "
                         "un-blocked AI write; may hold only the eyes on a package-locked follower).",
                         actor ? actor->GetFormID() : 0);
        }

        // Stopgap: the AI's headtrack write is not yet blocked, so re-apply each
        // tick. KNOWN-INCOMPLETE block, not a clean gate (INVARIANTS #2). The ONLY
        // channel that does per-tick work.
        void Tick(RE::Actor* actor) override { AssertLookUp(actor); }

        void Release(RE::Actor* actor) override {
            spdlog::info("[ch.5] 0x{:08X} released -- stop the stopgap re-assert; AI resumes its headtrack.",
                         actor ? actor->GetFormID() : 0);
        }
    };

}

APMF_REGISTER_CHANNEL(HeadtrackChannel);
