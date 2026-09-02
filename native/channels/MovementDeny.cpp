#include "PCH.h"
#include "core/Registry.h"
#include "core/Arbiter.h"

// ============================================================================
// Channel 1 -- MOVEMENT (DENY only). Suspend the actor's own locomotion so the
// package's movement never reaches the body, while the package stays current and
// keeps evaluating (design.md Section 5). Set ONCE, engine-honored, NO re-assert
// (Docs/INVARIANTS.md #1).
//
// Mechanism: Actor "don't move", bound through Address Library
// (RELOCATION_ID(SE,AE) -> resolves per runtime, no hardcoded call-site offset;
// IDs proven in the prototype/probe). This is the version-ROBUST deny that
// actually compiles in the pinned CommonLib rev.
//
// Why not MovementControllerNPC::SetAIDriven: that rev exposes NO named
// AI-driven setter -- only unnamed void(void) `Unk_0C/0D` vfuncs (see
// RE/M/MovementControllerNPC.h). Calling those blind is the documented
// blind-vtable CTD roulette, so the planner-input gate stays probe-gated and we
// use the bound don't-move deny instead. VR-refused (the reloc IDs are SE/AE).
//
// The movement PROMOTE feed (drive the body to a new goal) is probe-gated
// (CHANNEL-MAP ch.1 "promote GAP") and NOT built here.
// ============================================================================

namespace {

    namespace Native {
        void SetDontMove(RE::Actor* a_actor, bool a_dontMove) {
            using func_t = void (*)(RE::Actor*, bool);
            static REL::Relocation<func_t> func{ RELOCATION_ID(36490, 37489) };
            func(a_actor, a_dontMove);
        }
    }

    class MovementDenyChannel final : public apmf::Channel {
    public:
        const char* Name() const override { return "movement-deny"; }
        int         ChannelNo() const override { return 1; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4F, "Numpad1 : toggle movement DENY (don't-move, package stays current)" },
            };
            return keys;
        }

        void OnHotkey(std::uint32_t, RE::Actor* target) override {
            if (engaged.load()) { Release(target ? target : apmf::Arbiter::Get().CurrentTarget()); return; }
            if (REL::Module::IsVR()) { spdlog::warn("[ch.1] REFUSED -- VR (don't-move reloc IDs are SE/AE only)."); return; }
            if (!target) { spdlog::warn("[ch.1] REFUSED -- no gated target."); return; }
            Native::SetDontMove(target, true);          // deny the actor's locomotion at the movement layer
            engaged.store(true);
            spdlog::info("[ch.1] DENY engaged on 0x{:08X} -- SetDontMove(true). Package left current, its "
                         "locomotion suppressed. Set ONCE, no re-assert.", target->GetFormID());
        }

        void Release(RE::Actor* actor) override {
            if (!engaged.exchange(false)) return;
            if (actor && !REL::Module::IsVR()) Native::SetDontMove(actor, false);
            spdlog::info("[ch.1] DENY released -- SetDontMove(false), AI resumes locomotion.");
        }
    };

}

APMF_REGISTER_CHANNEL(MovementDenyChannel);
