#include "PCH.h"
#include "core/Log.h"
#include "core/Hook.h"
#include "core/Arbiter.h"

// ============================================================================
// The one central engine seat: Actor::Update(float) @ VIRTUAL index 0xAD
// (design.md Section 3). Patching the Character vtable ONCE routes every NPC's
// per-frame tick through the arbiter -- no per-actor hooks. Version-robust: a
// vtable index is a single source constant across runtimes (unlike a call-site
// offset), and True Directional Movement hooks this exact slot at scale.
// ============================================================================

// Win32 thread id for the [threadcheck] single-writer detector below. Declared
// by hand (Hook.cpp-local, mirrors AliasPkgProbe.cpp's 0x49 probe); pointer-free,
// no header conflict -- PCH does not pull in <Windows.h>.
extern "C" __declspec(dllimport) std::uint32_t __stdcall GetCurrentThreadId();

namespace apmf::hook {

    namespace {

        // ----------------------------------------------------------------------
        // [threadcheck]: instrumentation-only detector for the load-bearing
        // assumption behind INVARIANTS #12/#13 -- that the Character 0xAD seat
        // (every NPC's unlocked ControlMap::OnActorUpdate read) always runs on
        // the SAME thread as the PlayerCharacter 0xAD seat (the one that DRAINs
        // and mutates the map, apmf::Arbiter::Get().OncePerFrame() above). The
        // 0x49 probe proved a sibling Actor vfunc fires on 4+ threads; this
        // confirms (or refutes) 0xAD specifically before the map is trusted
        // lock-free. NO BEHAVIOR CHANGE: read-only, logs at most once, no lock.
        // ----------------------------------------------------------------------
        std::atomic<std::uint32_t> g_drainThreadId{ 0 };
        std::atomic<bool> g_threadCheckLogged{ false };

        struct CharacterUpdateHook {
            static void thunk(RE::Actor* a_this, float a_delta) {
                func(a_this, a_delta);                  // original AI tick FIRST
                apmf::Arbiter::Get().OnActorUpdate(a_this);

                // [threadcheck] -- see block comment above. Relaxed load; compares
                // against the reference thread id the player seat records once.
                const std::uint32_t drainTid = g_drainThreadId.load(std::memory_order_relaxed);
                const std::uint32_t hereTid = GetCurrentThreadId();
                if (drainTid != 0 && hereTid != drainTid && !g_threadCheckLogged.exchange(true)) {
                    spdlog::warn("[threadcheck] Character 0xAD (thread {}) != PlayerCharacter/Drain 0xAD "
                                 "(thread {}) -- INVARIANTS #12/#13 single-writer assumption is FALSE on "
                                 "this runtime; ControlMap needs a snapshot/RCU scheme before it can be "
                                 "trusted lock-free.", hereTid, drainTid);
                }
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

                // [threadcheck] -- record this seat's thread once as the reference
                // for the Character-side comparison above (relaxed store; this is
                // the authoritative/expected thread, not a race to detect).
                std::uint32_t expected = 0;
                g_drainThreadId.compare_exchange_strong(expected, GetCurrentThreadId(), std::memory_order_relaxed);
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

        spdlog::info("[hook] installed: Character + PlayerCharacter Update(0x{}). Central 0xAD arbiter "
                     "seat live for every NPC.", apmf::log::Hex(CharacterUpdateHook::idx, 0));
    }

}
