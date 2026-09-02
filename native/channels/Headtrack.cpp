#include "PCH.h"
#include "core/Registry.h"
#include "core/Arbiter.h"

// ============================================================================
// Channel 5 -- HEADTRACKING. Test facet: crane the head straight up.
//
// **KNOWN-INCOMPLETE BLOCK** (deck-tested 2026-09-02, folded in from the gap-probe
// + marth's gatekeeper reframe). APMF's job is to BE THE GATE: block the foreign
// input so nothing competes. This channel does NOT yet do that -- and the re-assert
// below is the SYMPTOM of a missing block, not an acceptable pattern (INVARIANTS
// #1). It is flagged incomplete, never called a clean gate.
//   - The AI writes MULTIPLE headtrack TYPES (default / combat / dialogue /
//     procedure). We only write the one point-based slot, so we do not block the
//     AI's own writes. On a normal follower that still reads as head+eyes; on a
//     PACKAGE-LOCKED follower (Cicero, pkg 0x0009BE51) the package keeps writing a
//     higher-priority type and reclaims the HEAD -- a visible per-tick war.
//   - Re-asserting each tick is a STOPGAP that papers over the un-blocked AI write.
//
// The REAL fix (not built): block the AI's headtrack write for this channel AT THE
// 0xAD hook -- skip/neutralize the AI's per-type write when APMF owns the channel,
// so there is nothing to fight and no re-assert. Until then this stays a
// known-incomplete block that can lose to an aggressive source. See INVARIANTS #2.
// ============================================================================

namespace {

    constexpr float kLookUpHeight = 300.0f;

    void AssertLookUp(RE::Actor* a) {
        auto* proc = a->GetActorRuntimeData().currentProcess;
        if (!proc) return;
        RE::NiPoint3 up = a->GetPosition();
        up.z += kLookUpHeight;                  // directly overhead -> look straight up
        proc->SetHeadtrackTarget(a, up);        // owns the point-based slot; not all types
    }

    class HeadtrackChannel final : public apmf::Channel {
    public:
        const char* Name() const override { return "headtrack"; }
        int         ChannelNo() const override { return 5; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x51, "Numpad3 : toggle look-straight-up (KNOWN-INCOMPLETE block)" },
            };
            return keys;
        }

        void OnHotkey(std::uint32_t, RE::Actor* target) override {
            if (engaged.load()) { Release(target ? target : apmf::Arbiter::Get().CurrentTarget()); return; }
            if (!target) { spdlog::warn("[ch.5] REFUSED -- no gated target."); return; }
            engaged.store(true);
            AssertLookUp(target);
            spdlog::info("[ch.5] engaged on 0x{:08X} -- KNOWN-INCOMPLETE block (look up). Re-assert is a "
                         "stopgap for the un-blocked AI write; may hold only the eyes on a package-locked "
                         "follower.", target->GetFormID());
        }

        // Stopgap: the AI's headtrack write is not yet blocked, so re-apply each
        // tick. This is a KNOWN-INCOMPLETE block, not a clean gate (INVARIANTS #2).
        void Tick(RE::Actor* actor) override { AssertLookUp(actor); }

        void Release(RE::Actor*) override {
            if (!engaged.exchange(false)) return;
            // Stop asserting; the AI resumes ownership of its headtrack types.
            spdlog::info("[ch.5] released -- stop the stopgap re-assert; AI resumes its headtrack.");
        }
    };

}

APMF_REGISTER_CHANNEL(HeadtrackChannel);
