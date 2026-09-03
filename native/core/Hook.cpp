#include "PCH.h"
#include "core/Log.h"
#include "core/Hook.h"
#include "core/Arbiter.h"
#include "core/ControlMap.h"

// ============================================================================
// The one central engine seat: Actor::Update(float) @ VIRTUAL index 0xAD
// (design.md Section 3). Patching the Character vtable ONCE routes every NPC's
// per-frame tick through the arbiter -- no per-actor hooks. Version-robust: a
// vtable index is a single source constant across runtimes (unlike a call-site
// offset), and True Directional Movement hooks this exact slot at scale.
// ============================================================================

// Win32 thread id for the [threadcheck] detector below. Declared by hand
// (Hook.cpp-local); pointer-free, no header conflict -- PCH does not pull in
// <Windows.h>.
extern "C" __declspec(dllimport) std::uint32_t __stdcall GetCurrentThreadId();

namespace apmf::hook {

    namespace {

        // ----------------------------------------------------------------------
        // [threadcheck]: instrumentation-only observer, RETIRED to informational
        // (2026-09-02, post-RCU). It originally detected whether the Character
        // 0xAD seat (every NPC's ControlMap::OnActorUpdate read) shares a thread
        // with the PlayerCharacter/Drain seat -- the load-bearing assumption
        // behind the OLD raw single-writer/unlocked-read model. It fired: a
        // Character seat ran on a different worker thread than Drain. That is no
        // longer a bug report -- ControlMap now publishes an RCU snapshot
        // (core/ControlMap.{h,cpp}) specifically so cross-thread reads are SAFE
        // regardless of which thread the Character seat lands on. This stays as a
        // one-time INFO log (not a warning) so a multi-thread runtime is still
        // visible in the log without crying wolf on every deck cycle.
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
                    spdlog::info("[threadcheck] 0xAD confirmed multi-thread (Character thread {} != "
                                 "PlayerCharacter/Drain thread {}) -- expected on this runtime; the RCU "
                                 "snapshot (ControlMap) makes cross-thread OnActorUpdate reads safe.",
                                 hereTid, drainTid);
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
                // Request/Release against a private working copy) and sweep unloaded
                // NPCs, then PUBLISH a new RCU snapshot iff something changed. The
                // per-NPC hot path (any thread) then reads that snapshot lock-free
                // (INVARIANTS #12/#13 -- RCU model, core/ControlMap.h).
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

        // Disclose, don't assume, whether the RCU snapshot pointer
        // (std::atomic<std::shared_ptr<const MapType>>, core/ControlMap.h) is
        // actually lock-free on this toolchain. NOT lock-free (spinlock-backed,
        // likely on MSVC) is ACCEPTABLE here: the control map holds only a handful
        // of controlled NPCs, so the critical section is a pointer copy, not a hot
        // loop -- but it must be visible in the log, not silently assumed, so a
        // heavier-than-expected fallback would be caught rather than shipped blind.
        const bool lockFree = apmf::ControlMap::SnapshotIsLockFree();
        spdlog::info("[hook] ControlMap RCU snapshot (atomic<shared_ptr<const MapType>>) is{} lock-free "
                     "on this toolchain.{}", lockFree ? "" : " NOT",
                     lockFree ? "" : " Spinlock-backed is expected/acceptable for this small map -- "
                                     "flagged here for the deck perf check, not a defect.");
    }

}
