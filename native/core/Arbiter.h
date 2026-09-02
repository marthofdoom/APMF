#pragma once
#include "APMF_API.h"

// ============================================================================
// APMF core -- the ARBITER: the front the engine hook, the input test surface, and
// the plugin lifecycle talk to. The real multi-NPC engine is core/ControlMap; the
// Arbiter delegates the per-frame drive to it and adds the crosshair TEST SURFACE.
//
// Layer 1 (design.md Section 2): the central Actor::Update(0xAD) hook calls
// OnActorUpdate for EVERY NPC (per-NPC hot path) and OncePerFrame once per frame
// (drain the client-API queue). The real client driver is the C-ABI API
// (APMF_API.h / core/ClientAPI); the hotkey surface here is for a tester and drives
// the SAME ControlMap through the SAME enqueue path, so there is one control path.
//
// MULTI-NPC TEST SURFACE. A hotkey ADDS the crosshair-aimed NPC to the controlled
// set on that key's channel (press again to remove it); a dedicated key releases
// ALL controlled NPCs. So a tester can freeze/bias several different followers at
// once and confirm each is held independently.
// ============================================================================

namespace apmf {

    class Arbiter {
    public:
        static Arbiter& Get();

        // From the 0xAD hook, for EVERY actor. Delegates to the ControlMap hot path.
        void OnActorUpdate(RE::Actor* actor);

        // Once per frame (from the PlayerCharacter 0xAD seat, game thread): drain
        // the client-API request queue and sweep unloaded controlled NPCs.
        void OncePerFrame();

        // Route a test-surface hotkey: toggle the aimed NPC on that key's channel,
        // or release-all on the dedicated key.
        void DispatchHotkey(std::uint32_t code);

        // Release every controlled NPC (disengage-all / kPreLoadGame).
        void ReleaseAll(const char* why);

    private:
        // Resolve the actor currently under the crosshair (null if none / player).
        RE::Actor* CrosshairActor() const;

        // Test-surface claims: key = (FormID<<32 | intent) -> the handle we hold.
        std::unordered_map<std::uint64_t, APMF_API::Handle> m_testHandles;
    };

}
