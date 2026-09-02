#include "PCH.h"
#include "core/Registry.h"
#include "core/Arbiter.h"

// ============================================================================
// Channel 5 -- HEADTRACKING. Test facet: crane the head straight up.
//
// **OVERRIDE-WITH-HOLD, NOT a clean source-gate** (deck-tested 2026-09-02, folded
// in from the gap-probe). Unlike the AV / casting-selection channels, this one is
// NOT a true input-gate:
//   - The AI writes MULTIPLE headtrack TYPES (default / combat / dialogue /
//     procedure). Owning the one point-based slot only holds PART of it. On a
//     normal follower you get head+eyes; on a PACKAGE-LOCKED follower (e.g.
//     Cicero, pkg 0x0009BE51) the package reclaims the HEAD via a higher-priority
//     type and we hold only the EYES -- a visible per-tick war.
//   - So we RE-ASSERT every tick to hold authority. That is the design's flagged
//     exception (design.md §1a rule 3): an override we must keep re-applying, not
//     a source we deny once. It CAN LOSE to an aggressive/package-locked source.
//
// This is why the module is honest about being override-with-hold: a real
// source-gate here would require owning/clearing ALL headtrack types (or detecting
// and owning the currently-winning type) each tick. Left as documented override
// until that typed-ownership API is pinned. See Docs/INVARIANTS.md #2.
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
                { 0x51, "Numpad3 : toggle look-straight-up (override-with-hold)" },
            };
            return keys;
        }

        void OnHotkey(std::uint32_t, RE::Actor* target) override {
            if (engaged.load()) { Release(target ? target : apmf::Arbiter::Get().CurrentTarget()); return; }
            if (!target) { spdlog::warn("[ch.5] REFUSED -- no gated target."); return; }
            engaged.store(true);
            AssertLookUp(target);
            spdlog::info("[ch.5] engaged on 0x{:08X} -- OVERRIDE-with-hold (look up), re-asserted each tick. "
                         "May only hold the eyes (not the head) on a package-locked follower.",
                         target->GetFormID());
        }

        // Override-with-hold: re-apply each tick. This is NOT a clean source-gate.
        void Tick(RE::Actor* actor) override { AssertLookUp(actor); }

        void Release(RE::Actor*) override {
            if (!engaged.exchange(false)) return;
            // Stop asserting; the AI resumes ownership of its headtrack types.
            spdlog::info("[ch.5] released -- stop overriding the headtrack slot; AI resumes.");
        }
    };

}

APMF_REGISTER_CHANNEL(HeadtrackChannel);
