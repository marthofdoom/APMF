#include "PCH.h"
#include "core/Hook.h"
#include "core/Arbiter.h"

// ============================================================================
// The one central engine seat: Actor::Update(float) @ VIRTUAL index 0xAD
// (design.md Section 3). Patching the Character vtable ONCE routes every NPC's
// per-frame tick through the arbiter -- no per-actor hooks. Version-robust: a
// vtable index is a single source constant across runtimes (unlike a call-site
// offset), and True Directional Movement hooks this exact slot at scale.
// ============================================================================

namespace apmf::hook {

    namespace {

        struct CharacterUpdateHook {
            static void thunk(RE::Actor* a_this, float a_delta) {
                func(a_this, a_delta);                  // original AI tick FIRST
                apmf::Arbiter::Get().OnActorUpdate(a_this);
            }
            static inline REL::Relocation<decltype(thunk)> func;
            static constexpr std::size_t idx = 0x0AD;   // Actor::Update(float)
        };

        struct PlayerUpdateHook {
            static void thunk(RE::PlayerCharacter* a_this, float a_delta) {
                func(a_this, a_delta);
                // The player ticks exactly ONCE per frame on the game thread -- the
                // right seat to DRAIN the client-API request queue (apply enqueued
                // Request/Release, mutating the control map on the game thread only)
                // and sweep unloaded NPCs. The per-NPC hot path then reads the map
                // lock-free (single-writer model, INVARIANTS #12).
                apmf::Arbiter::Get().OncePerFrame();
                static bool s_first = true;
                if (s_first) { s_first = false; spdlog::info("[hook] player Update seat live (0xAD firing; per-frame drain armed)"); }
            }
            static inline REL::Relocation<decltype(thunk)> func;
            static constexpr std::size_t idx = 0x0AD;
        };

        std::atomic<bool> g_installed{ false };

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[hook] VR runtime -- 0xAD index unverified for VR; hooks NOT installed.");
            return;
        }
        if (g_installed.exchange(true)) return;

        REL::Relocation<std::uintptr_t> charVtbl{ RE::VTABLE_Character[0] };
        CharacterUpdateHook::func = charVtbl.write_vfunc(CharacterUpdateHook::idx, CharacterUpdateHook::thunk);

        REL::Relocation<std::uintptr_t> pcVtbl{ RE::VTABLE_PlayerCharacter[0] };
        PlayerUpdateHook::func = pcVtbl.write_vfunc(PlayerUpdateHook::idx, PlayerUpdateHook::thunk);

        spdlog::info("[hook] installed: Character + PlayerCharacter Update(0x{:X}). Central 0xAD arbiter "
                     "seat live for every NPC.", CharacterUpdateHook::idx);
    }

}
