#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 6 -- COMBAT-TARGET, now HELD (was STEER-only). The client names the
// target via RequestEx's APMF_Param.form (a v2 addition); Engage seeds combat with
// Actor::StartCombat (Address-Library bound, unbound in CommonLib -- INVARIANTS #8),
// and Tick RE-ASSERTS it so the target is HELD for the life of the claim.
//
// **KNOWN-INCOMPLETE block** (INVARIANTS #2). The clean gate would neutralise the
// threat re-selector's write at the 0xAD hook; that PIN is the documented ch.6 GAP
// (CHANNEL-MAP "steer DOCUMENTED; PIN GAP"). Until it is built, this is the
// PRAGMATIC FILL: each per-NPC tick we read the actor's live `currentCombatTarget`
// and, ONLY when it has drifted off our target (the threat system re-chose -- it
// re-selects on a multi-second cadence -- or combat has not started yet), re-call
// StartCombat. That makes the hold DRIFT-TRIGGERED, not a blind per-frame hammer:
// while it is holding, a tick costs one handle compare and nothing else (no
// StartCombat, no thrash); a drift is corrected within a single frame, faster than
// the re-selector's cadence, so the target stays pinned. Same re-assert shape ch.5
// headtrack uses; it can still lose for one frame to an aggressive re-selector,
// hence "known-incomplete". (StartCombat, not a direct combatController->targetHandle
// write, is used deliberately: it keeps APMF off the AE +8 CombatController layout
// hazard and it also INITIATES combat when there is none, which "make her fight this
// foe" wants.)
//
// RELEASE RELINQUISHES, IT DOES NOT StopCombat. A client releases this claim
// CONSTANTLY (its gambit yields to another rule, the client's expiry sweep, a target
// switch), so StopCombat on release would repeatedly yank a follower out of an
// ongoing fight and flicker Stop->Start on a switch. Instead Release just stops
// re-asserting: the threat system keeps the fight if the foe is still hostile, and
// ends it for its own reasons otherwise (which APMF must not override). This is the
// release pattern for every "steer a facet" channel.
//
// Target = param.form (a specific Actor); with NO param (form == 0, e.g. the
// NumpadMinus test hotkey) it falls back to the PLAYER as a self-evident demo target.
// VR-refused (the StartCombat reloc IDs are SE/AE only).
// ============================================================================

namespace {

    namespace Native {
        // Actor::StartCombat(target) -- seed combat against a_target.
        void StartCombat(RE::Actor* a_actor, RE::Actor* a_target) {
            using func_t = void (*)(RE::Actor*, RE::Actor*);
            static REL::Relocation<func_t> func{ RELOCATION_ID(37608, 38561) };
            func(a_actor, a_target);
        }
    }

    class CombatTargetChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "combat-target"; }
        int              ChannelNo() const override { return 6; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_CombatTarget; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4A, "NumpadMinus : HOLD combat target on the player (no-param demo)" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            if (!actor || REL::Module::IsVR()) return;   // reloc IDs are SE/AE only
            const RE::FormID want = ResolveTargetID(param);
            if (want == 0) return;
            m_target[id] = want;
            AssertTarget(id, actor);
            spdlog::info("[ch.6] 0x{} HOLD -- StartCombat(0x{}); Tick re-asserts on drift "
                         "(known-incomplete PIN fill).", apmf::log::Hex(id), apmf::log::Hex(want));
        }

        // A new higher-basis claim (or a new winner after the owner released) wants a
        // DIFFERENT target: switch the held target and re-seed at once.
        void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            if (!actor || REL::Module::IsVR()) return;
            const RE::FormID want = ResolveTargetID(param);
            if (want == 0) return;
            m_target[id] = want;
            AssertTarget(id, actor);
            spdlog::info("[ch.6] 0x{} target RE-POINTED to 0x{} (new owner).",
                         apmf::log::Hex(id), apmf::log::Hex(want));
        }

        // Drift-triggered re-assert: hold the target while the claim lives. Cheap when
        // already holding (a handle compare); re-seeds only when the engine drifted off
        // our target or has no combat target yet.
        void Tick(RE::FormID id, RE::Actor* actor) override {
            if (!actor || REL::Module::IsVR()) return;
            auto it = m_target.find(id);
            if (it == m_target.end()) return;
            auto* want = RE::TESForm::LookupByID<RE::Actor>(it->second);
            if (!want) return;   // target gone; the claim's owner Releases it
            auto cur = actor->GetActorRuntimeData().currentCombatTarget.get();   // NiPointer<Actor>
            if (cur.get() == want) return;   // already on our target -> no work, no thrash
            Native::StartCombat(actor, want);   // drifted / not started -> re-seed
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            if (auto it = m_target.find(id); it != m_target.end()) {
                // RELINQUISH the hold by ceasing to re-assert -- do NOT StopCombat.
                // A client releases this claim CONSTANTLY (a cast gambit yields to
                // another rule, the 500ms expiry sweep, a target switch), and
                // StopCombat would yank the follower out of an ongoing fight every
                // time and flicker StopCombat->StartCombat on a switch. The threat
                // system keeps the fight going on its own if the foe is still
                // hostile; if it is not, the engine ends combat for its own reasons
                // (which APMF must not override -- "commanding WHICH foe is ours;
                // commanding THAT there is a foe is not"). So we simply stop steering:
                // drop the per-NPC target and the next tick no longer re-asserts.
                // (This is the release pattern for every steer-a-facet gambit.)
                spdlog::info("[ch.6] 0x{} released -- relinquished (no StopCombat).", apmf::log::Hex(id));
                m_target.erase(it);   // drop per-NPC state keyed by id (actor may be null)
            }
        }

    private:
        // The client's chosen target (param.form) if given, else the player.
        static RE::FormID ResolveTargetID(const APMF_API::APMF_Param& param) {
            if (param.form) return param.form;
            auto* player = RE::PlayerCharacter::GetSingleton();
            return player ? player->GetFormID() : 0;
        }

        void AssertTarget(RE::FormID id, RE::Actor* actor) {
            auto it = m_target.find(id);
            if (it == m_target.end()) return;
            if (auto* want = RE::TESForm::LookupByID<RE::Actor>(it->second))
                Native::StartCombat(actor, want);
        }

        std::unordered_map<RE::FormID, RE::FormID> m_target;   // id -> desired target FormID (game-thread only)
    };

}

APMF_REGISTER_CHANNEL(CombatTargetChannel);
