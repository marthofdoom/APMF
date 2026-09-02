#include "PCH.h"
#include "core/Registry.h"
#include "core/Arbiter.h"

// ============================================================================
// Channel 1 -- MOVEMENT (DENY only). The clean source-gate: suspend the actor's
// own package planner via MovementControllerNPC::SetAIDriven(false) so the AI
// never produces a locomotion command. NOTHING to fight, no re-assert (design.md
// Section 1a, rule 3). Release re-enables AI driving.
//
// This is the DENY half only. The movement PROMOTE feed (drive the body toward a
// new goal) is probe-gated (IMovementDirectControl is unnamed -- CHANNEL-MAP ch.1
// "promote GAP") and NOT built here.
//
// PACKAGE COHERENCE: the package stays current and keeps evaluating; only its
// locomotion input is suspended. No substitution (design.md Section 5).
// ============================================================================

namespace {

    RE::MovementControllerNPC* GetController(RE::Actor* a) {
        if (!a) return nullptr;
        // Actor::movementController (design.md Section 4). Version-robust via the
        // CommonLib accessor, not a raw offset.
        return a->GetActorRuntimeData().movementController.get();
    }

    class MovementDenyChannel final : public apmf::Channel {
    public:
        const char* Name() const override { return "movement-deny"; }
        int         ChannelNo() const override { return 1; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4F, "Numpad1 : toggle movement DENY (SetAIDriven false)" },
            };
            return keys;
        }

        void OnHotkey(std::uint32_t, RE::Actor* target) override {
            if (engaged.load()) { Release(target ? target : apmf::Arbiter::Get().CurrentTarget()); return; }
            if (!target) { spdlog::warn("[ch.1] REFUSED -- no gated target."); return; }
            auto* mc = GetController(target);
            if (!mc) { spdlog::warn("[ch.1] REFUSED -- no MovementControllerNPC on 0x{:08X}.", target->GetFormID()); return; }
            mc->SetAIDriven(false);                     // gate the planner OFF at the source
            engaged.store(true);
            spdlog::info("[ch.1] DENY engaged on 0x{:08X} -- SetAIDriven(false). Planner suspended, package "
                         "left current. Set ONCE, no re-assert.", target->GetFormID());
        }

        void Release(RE::Actor* actor) override {
            if (!engaged.exchange(false)) return;
            if (auto* mc = GetController(actor)) mc->SetAIDriven(true);
            spdlog::info("[ch.1] DENY released -- SetAIDriven(true), AI resumes locomotion.");
        }
    };

}

APMF_REGISTER_CHANNEL(MovementDenyChannel);
