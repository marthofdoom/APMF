#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 1 -- MOVEMENT, FULL BLOCK (a clean stand-still). Reference channel done
// right (ROADMAP Phase 1). v0.1.0's SetDontMove alone blocks TRANSLATION but not
// the move INTENT: the package keeps trying to move, so the follower runs-in-place
// and teleport-snaps on release. This blocks the intent AT THE SOURCE too, so she
// simply stands still -- no walking, no run-in-place animation, no teleport.
//
// Mechanism (both layers, no re-assert -- set once at Engage):
//   1. KeepOffsetFromActor(SELF, offset 0) -- parks her movement GOAL at her own
//      position. The planner sees "already there" and produces no locomotion
//      command, so there is nothing to run-in-place and nothing to snap back from.
//      This is the block-the-source layer (design.md Section 1a rule 3).
//   2. SetDontMove(true) -- belt-and-suspenders translation lock at the mover.
// Both are bound through Address Library (RELOCATION_ID(SE,AE) -> resolves per
// runtime, no hardcoded call-site offset). The package stays CURRENT and keeps
// evaluating the whole time (design.md Section 5) -- APMF never substitutes it.
//
// Why NOT MovementControllerNPC::SetAIDriven: the pinned rev exposes NO named
// AI-driven setter -- only unnamed void(void) Unk_0C/0D vfuncs; calling those blind
// is the documented CTD roulette (INVARIANTS #8), so we use the bound calls above.
// VR-refused (the reloc IDs are SE/AE only).
// ============================================================================

namespace {

    // Address Library IDs (SE, AE). None of these is bound in the pinned CommonLib
    // rev (verified against the fork's Actor.h -- CommonLibSSE-NG does not vendor
    // KeepOffsetFromActor). VERIFIED IDs: SetDontMove (36490, 37489) is the known-good
    // calibration pair; KeepOffsetFromActor (36870, 37894) and ClearKeepOffsetFromActor
    // (36871, 37895) were cross-checked against shipping SKSE source (Adventurers-Like-
    // You Util.h) that carries the IDENTICAL signature AND reproduces the SetDontMove
    // anchor verbatim, with zero conflicting values found anywhere -- see INVARIANTS #8.
    // VR has no sourced IDs -> VR-refused.
    namespace Native {
        // Actor::SetDontMove(bool) -- suspend the mover's translation.
        void SetDontMove(RE::Actor* a_actor, bool a_dontMove) {
            using func_t = void (*)(RE::Actor*, bool);
            static REL::Relocation<func_t> func{ RELOCATION_ID(36490, 37489) };
            func(a_actor, a_dontMove);
        }
        // Actor::KeepOffsetFromActor(target, offset, angle, catchUp, follow) --
        // parks the movement goal relative to `target` (here: SELF, offset 0).
        void KeepOffsetFromActor(RE::Actor* a_actor, const RE::ActorHandle& a_target,
                                 const RE::NiPoint3& a_offset, const RE::NiPoint3& a_angle,
                                 float a_catchUpRadius, float a_followRadius) {
            using func_t = void (*)(RE::Actor*, const RE::ActorHandle&, const RE::NiPoint3&,
                                    const RE::NiPoint3&, float, float);
            static REL::Relocation<func_t> func{ RELOCATION_ID(36870, 37894) };
            func(a_actor, a_target, a_offset, a_angle, a_catchUpRadius, a_followRadius);
        }
        // Actor::ClearKeepOffsetFromActor() -- release the parked goal.
        void ClearKeepOffsetFromActor(RE::Actor* a_actor) {
            using func_t = void (*)(RE::Actor*);
            static REL::Relocation<func_t> func{ RELOCATION_ID(36871, 37895) };
            func(a_actor);
        }
    }

    class MovementDenyChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "movement-block"; }
        int              ChannelNo() const override { return 1; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_MovementBlock; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4F, "Numpad1 : toggle FULL movement block (clean stand-still)" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* actor) override {
            if (!actor || REL::Module::IsVR()) return;   // reloc IDs are SE/AE only
            const RE::ActorHandle self = actor->GetHandle();
            const RE::NiPoint3    zero{ 0.0f, 0.0f, 0.0f };
            Native::KeepOffsetFromActor(actor, self, zero, zero, 20.0f, 10.0f);  // goal := my own spot
            Native::SetDontMove(actor, true);                                     // + lock translation
            spdlog::info("[ch.1] 0x{} FULL block -- KeepOffsetFromActor(self,0) + SetDontMove(true). "
                         "Move intent nulled at the source: stands still, no run-in-place, no snap.", apmf::log::Hex(id));
        }

        void Release(RE::FormID id, RE::Actor* actor) override {
            if (!actor || REL::Module::IsVR()) return;   // no persisted state to clean if actor is gone
            Native::ClearKeepOffsetFromActor(actor);
            Native::SetDontMove(actor, false);
            spdlog::info("[ch.1] 0x{} block released -- goal + mover restored, AI resumes locomotion.", apmf::log::Hex(id));
        }
    };

}

APMF_REGISTER_CHANNEL(MovementDenyChannel);
