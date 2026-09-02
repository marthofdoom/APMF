#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 6 -- COMBAT-TARGET, HELD (the true ch.6 PIN). The client names the target
// via RequestEx's APMF_Param.form (v2) and re-points it via Repoint (v3). The target
// is COMMANDED + HELD for the life of the claim by the MEASURED mechanism (see below).
//
// TWO ENGINE OPERATIONS, EACH FOR ITS ONE LEGIT JOB:
//   1. INITIATE -- Actor::StartCombat(actor, target, nullptr), used ONLY when the
//      actor is not already in combat, to start the fight. It is the po3 relocation
//      CommonLib does not bind (INVARIANTS #8). **Its signature is
//      `bool(RE::Actor*, RE::Actor*, void*)` -- THREE args.** (An earlier 2-arg
//      wrapper here left the engine reading the 3rd param from a garbage register and
//      dereferencing it -> a hard AV in StartCombat. The 3rd arg is mandatory.) Raw
//      StartCombat was ALSO measured NOT to RETAIN a target across the threat
//      re-selector's re-picks, so it is NOT used to hold -- only to start.
//   2. HOLD / COMMAND / RE-POINT -- a compare-and-write of the actor's
//      `GetActorRuntimeData().currentCombatTarget` (an AIProcess-runtime ActorHandle).
//      This is the field the target selector re-reads; writing it commands the foe and
//      holds it against the re-selector's drift. It lives on the actor's runtime data,
//      NOT on the CombatController, so it is CLEAR of the AE +8 (<0x68) layout hazard
//      that the CombatController members carry -- no offset assert needed.
//
// So each per-NPC Tick: if the live currentCombatTarget already IS our target, do
// nothing (a handle compare, no write, no thrash); otherwise correct it with a single
// compare-and-write (and StartCombat first iff the actor is not yet in combat). This
// is the engine's own cadence -- a handle compare-and-write, never a StopCombat/
// StartCombat reset. It is the "true PIN" the CHANNEL-MAP marked a GAP for ch.6.
//
// RELEASE RELINQUISHES, IT DOES NOT StopCombat and does NOT clear currentCombatTarget.
// A client releases this claim CONSTANTLY (a gambit yields, an expiry sweep, a target
// switch), so undoing on release would yank a follower out of an ongoing fight and
// flicker on a switch. Release just stops re-asserting: the threat system keeps the
// fight if the foe is still hostile, and ends it for its own reasons otherwise (which
// APMF must not override). This is the release pattern for every "steer a facet"
// channel (INVARIANTS #5a).
//
// Target = param.form (a specific Actor); with NO param (form == 0, e.g. the
// NumpadMinus test hotkey) it falls back to the PLAYER as a self-evident demo target.
// VR-refused (the StartCombat reloc IDs are SE/AE only).
// ============================================================================

namespace {

    namespace Native {
        // Actor::StartCombat(actor, target, unk3) -- START a fight against a_target.
        // po3's published relocation, unbound in CommonLib (INVARIANTS #8). The REAL
        // signature takes THREE args (bool(Actor*, Actor*, void*)); the 3rd is passed
        // nullptr. Omitting it faults inside the engine (garbage 3rd register deref).
        // Used ONLY to initiate combat -- holding is the currentCombatTarget write.
        bool StartCombat(RE::Actor* a_actor, RE::Actor* a_target) {
            using func_t = bool (*)(RE::Actor*, RE::Actor*, void*);
            static REL::Relocation<func_t> func{ RELOCATION_ID(37608, 38561) };
            return func(a_actor, a_target, nullptr);
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
            spdlog::info("[ch.6] 0x{} HOLD -- command target 0x{} (StartCombat to initiate + "
                         "currentCombatTarget write to hold).", apmf::log::Hex(id), apmf::log::Hex(want));
        }

        // A new higher-basis claim, or a Repoint on the owning claim: switch the held
        // target and command it at once (same continuous claim, no release/re-request).
        void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            if (!actor || REL::Module::IsVR()) return;
            const RE::FormID want = ResolveTargetID(param);
            if (want == 0) return;
            m_target[id] = want;
            AssertTarget(id, actor);
            spdlog::info("[ch.6] 0x{} target RE-POINTED to 0x{}.", apmf::log::Hex(id), apmf::log::Hex(want));
        }

        // Drift-triggered hold: cheap when already on our target (a handle compare, no
        // write); on drift, one compare-and-write of currentCombatTarget corrects it.
        void Tick(RE::FormID id, RE::Actor* actor) override {
            if (!actor || REL::Module::IsVR()) return;
            auto it = m_target.find(id);
            if (it == m_target.end()) return;
            auto* want = RE::TESForm::LookupByID<RE::Actor>(it->second);
            if (!want) return;   // target gone; the claim's owner Releases it
            auto& rt = actor->GetActorRuntimeData();
            if (rt.currentCombatTarget.get().get() == want) return;   // holding -> no work, no thrash
            Command(actor, want);   // drifted / not started -> correct it
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            if (auto it = m_target.find(id); it != m_target.end()) {
                // RELINQUISH by ceasing to re-assert -- do NOT StopCombat, do NOT clear
                // currentCombatTarget (releases are constant; undoing would yank the
                // follower out of a live fight). The threat system keeps the fight if
                // the foe is still hostile and ends it otherwise (INVARIANTS #5a).
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

        // COMMAND + HOLD the target: initiate combat iff not already fighting, then
        // compare-and-write the AIProcess currentCombatTarget (the measured pin field;
        // clear of the CombatController AE +8 hazard).
        static void Command(RE::Actor* actor, RE::Actor* want) {
            if (!actor->IsInCombat()) Native::StartCombat(actor, want);   // INITIATE only
            auto& rt = actor->GetActorRuntimeData();
            if (rt.currentCombatTarget.get().get() != want)
                rt.currentCombatTarget = want->GetHandle();              // HOLD / command
        }

        void AssertTarget(RE::FormID id, RE::Actor* actor) {
            auto it = m_target.find(id);
            if (it == m_target.end()) return;
            if (auto* want = RE::TESForm::LookupByID<RE::Actor>(it->second))
                Command(actor, want);
        }

        std::unordered_map<RE::FormID, RE::FormID> m_target;   // id -> desired target FormID (game-thread only)
    };

}

APMF_REGISTER_CHANNEL(CombatTargetChannel);
