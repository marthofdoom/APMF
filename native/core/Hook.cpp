#include "PCH.h"
#include "core/Sources.h"   // Harbinger 1a: seat census
#include "core/Allowance.h"   // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Log.h"
#include "core/Hook.h"
#include "core/Arbiter.h"
#include "core/ControlMap.h"
#include "core/Clock.h"

#include <exception>
#include <iterator>
#include <string>

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

        // ---- P0 overlap probe (Hook.h "P0"). Threads inside an NPC's Actor::Update (0xAD) / inside the
        // outermost Harbinger layer of Character::UpdateCombat (0xE4) right now. Relaxed RMW: a count, not
        // a lock; a sample is a snapshot that may be one instruction stale, which is what "overlap" means.
        std::atomic<std::int32_t> g_inNpcUpdate{ 0 };
        std::atomic<std::int32_t> g_inCombatUpdate{ 0 };
        struct SiteCount {
            std::atomic<std::uint64_t> samples{ 0 }, npc{ 0 }, combat{ 0 };   // per window
            std::uint64_t totalSamples = 0, totalNpc = 0, totalCombat = 0;      // session (main only)
        };
        SiteCount     g_site[static_cast<std::size_t>(Site::kCount)];
        std::uint64_t g_overlapLastMs = 0;   // main only
        constexpr std::uint64_t kOverlapEveryMs = 60000;
        constexpr const char*   kSiteName[] = { "pump begin", "pump end", "StartCombat", "RemoveTarget",
                                                "StopCombat on main", "anchor move", "save flush" };
        static_assert(std::size(kSiteName) == static_cast<std::size_t>(Site::kCount));

        struct NpcUpdateCount {
            NpcUpdateCount() { g_inNpcUpdate.fetch_add(1, std::memory_order_relaxed); }
            ~NpcUpdateCount() { g_inNpcUpdate.fetch_sub(1, std::memory_order_relaxed); }
        };

        // ---- M5/H (stability audit 2026-11-16): the per-frame hooks' exception guard. An exception that
        // escapes Harbinger's part of a hook would unwind through the engine's own update (no handler there:
        // a CTD whose stack points at the engine). Each stage is caught where it enters Harbinger, logged
        // ERROR naming the stage, and that stage is STOPPED for the rest of the session -- never retried,
        // never silently skipped (principle 7): the log says exactly what stopped. The engine's original
        // always runs (it is outside the try). C++ exceptions only (/EHsc): an access violation is not one.
        std::atomic<bool> g_actorStageOff{ false };   // Arbiter::OnActorUpdate (every NPC)
        std::atomic<bool> g_frameStageOff{ false };   // Arbiter::OncePerFrame (the drain, the pump, every poll)

        void StageFault(std::atomic<bool>& a_off, const char* a_stage, const char* a_what) {
            if (a_off.exchange(true)) return;
            spdlog::error("[hook] EXCEPTION in {} ({}) -- that stage is STOPPED for the rest of this session (no retry, "
                          "never masked): the engine's own update still runs, Harbinger's part of it does not. Every "
                          "claim it would serve now stands still. Report this log.",
                          a_stage, a_what ? a_what : "a non-standard exception");
        }

        struct CharacterUpdateHook {
            static void thunk(RE::Actor* a_this, float a_delta) {
                const NpcUpdateCount counted;           // P0: this thread is inside an NPC's update
                func(a_this, a_delta);                  // original AI tick FIRST
                if (!g_actorStageOff.load(std::memory_order_relaxed)) {
                    try {
                        apmf::Arbiter::Get().OnActorUpdate(a_this);
                    } catch (const std::exception& e) {
                        StageFault(g_actorStageOff, "the Character 0xAD seat (Arbiter::OnActorUpdate)", e.what());
                    } catch (...) {
                        StageFault(g_actorStageOff, "the Character 0xAD seat (Arbiter::OnActorUpdate)", nullptr);
                    }
                }

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
                Sample(Site::kPumpBegin);   // P0
                if (!g_frameStageOff.load(std::memory_order_relaxed)) {
                    try {
                        apmf::Arbiter::Get().OncePerFrame(a_delta);   // the engine's own world-time frame step (Arbiter.h)
                    } catch (const std::exception& e) {
                        StageFault(g_frameStageOff, "the PlayerCharacter 0xAD seat (Arbiter::OncePerFrame)", e.what());
                    } catch (...) {
                        StageFault(g_frameStageOff, "the PlayerCharacter 0xAD seat (Arbiter::OncePerFrame)", nullptr);
                    }
                }
                Sample(Site::kPumpEnd);     // P0
                OverlapHeartbeat();
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
        // APMF-B45: set once Install() has refused the seat (never cleared: nothing re-runs it).
        std::atomic<const char*> g_refusedReason{ nullptr };

    }

    const char* RefusedReason() { return g_refusedReason.load(std::memory_order_acquire); }

    void Sample(Site a_site) {
        auto& c = g_site[static_cast<std::size_t>(a_site)];
        c.samples.fetch_add(1, std::memory_order_relaxed);
        if (g_inNpcUpdate.load(std::memory_order_relaxed) > 0) c.npc.fetch_add(1, std::memory_order_relaxed);
        if (g_inCombatUpdate.load(std::memory_order_relaxed) > 0) c.combat.fetch_add(1, std::memory_order_relaxed);
    }

    void CombatUpdateEnter() { g_inCombatUpdate.fetch_add(1, std::memory_order_relaxed); }
    void CombatUpdateLeave() { g_inCombatUpdate.fetch_sub(1, std::memory_order_relaxed); }

    void OverlapHeartbeat() {
        const std::uint64_t now = apmf::clock::MonotonicMs();
        if (g_overlapLastMs == 0) { g_overlapLastMs = now; return; }
        if (now - g_overlapLastMs < kOverlapEveryMs) return;
        const std::uint64_t secs = (now - g_overlapLastMs) / 1000;
        g_overlapLastMs = now;
        std::string win, tot;
        std::uint64_t anyNpc = 0, anyCombat = 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(Site::kCount); ++i) {
            auto& c = g_site[i];
            const auto n = c.samples.exchange(0, std::memory_order_relaxed);
            const auto a = c.npc.exchange(0, std::memory_order_relaxed);
            const auto b = c.combat.exchange(0, std::memory_order_relaxed);
            c.totalSamples += n;
            c.totalNpc += a;
            c.totalCombat += b;
            if (i >= static_cast<std::size_t>(Site::kStartCombat)) { anyNpc += a; anyCombat += b; }
            win += fmt::format("{}{} {}/{}/{}", win.empty() ? "" : "; ", kSiteName[i], n, a, b);
            tot += fmt::format("{}{} {}/{}/{}", tot.empty() ? "" : "; ", kSiteName[i], c.totalSamples, c.totalNpc,
                               c.totalCombat);
        }
        spdlog::info("[overlap] {} s (P0, passive; samples / an NPC Actor::Update 0xAD running on another thread / a "
                     "Character::UpdateCombat 0xE4 running): {}. Session: {}.{}",
                     secs, win, tot,
                     anyNpc || anyCombat
                         ? " OVERLAP at a Harbinger combat call: a main-thread combat write met an NPC update in flight "
                           "(H1 graded SEV-2)."
                         : "");
    }

    bool OnMainThread() {
        const std::uint32_t mainTid = g_drainThreadId.load(std::memory_order_relaxed);
        return mainTid != 0 && mainTid == GetCurrentThreadId();
    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[hook] VR runtime -- 0xAD index unverified for VR; hooks NOT installed.");
            g_refusedReason.store("VR runtime (the 0xAD arbiter seat is not installed)", std::memory_order_release);
            return;
        }
        if (!allowance::RuntimeSupported()) {   // G1: exact build, never the 1.6 path by bucket
            spdlog::error("[hook] runtime {} is not exactly 1.6.1170, 1.5.97 or 1.7.104 -- the 0xAD arbiter seat is "
                          "verified on those three only; hooks NOT installed (REFUSED).",
                          REL::Module::get().version().string("."));
            g_refusedReason.store("unsupported runtime (the 0xAD arbiter seat is not installed)", std::memory_order_release);
            return;
        }
        if (g_installed.exchange(true)) return;

        REL::Relocation<std::uintptr_t> charVtbl{ RE::VTABLE_Character[0] };
        REL::Relocation<std::uintptr_t> pcVtblCheck{ RE::VTABLE_PlayerCharacter[0] };
        // mit-3.7 F1: both vtables must pass the self-check, or neither hook goes in
        // (the 0xAD arbiter seat is one seat served by two vtables).
        if (!allowance::SeatVerified(charVtbl.address(), "Hook.Character.Update") ||
            !allowance::SeatVerified(pcVtblCheck.address(), "Hook.PlayerCharacter.Update")) {
            g_installed.store(false);
            spdlog::error("[hook] 0xAD arbiter seat NOT installed (self-check refused a vtable).");
            g_refusedReason.store("the self-check refused the 0xAD arbiter seat's vtable", std::memory_order_release);
            return;
        }
        CharacterUpdateHook::func = charVtbl.write_vfunc(CharacterUpdateHook::idx, CharacterUpdateHook::thunk);
        apmf::sources::NoteVfunc(apmf::sources::Facet::kCore, "Hook.Character.Update", charVtbl.address(),
                                 CharacterUpdateHook::idx, SKSE::stl::unrestricted_cast<std::uintptr_t>(&CharacterUpdateHook::thunk),
                                 CharacterUpdateHook::func.address());

        REL::Relocation<std::uintptr_t> pcVtbl{ RE::VTABLE_PlayerCharacter[0] };
        PlayerUpdateHook::func = pcVtbl.write_vfunc(PlayerUpdateHook::idx, PlayerUpdateHook::thunk);
        apmf::sources::NoteVfunc(apmf::sources::Facet::kCore, "Hook.PlayerCharacter.Update", pcVtbl.address(),
                                 PlayerUpdateHook::idx, SKSE::stl::unrestricted_cast<std::uintptr_t>(&PlayerUpdateHook::thunk),
                                 PlayerUpdateHook::func.address());

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
