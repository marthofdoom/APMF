#include "PCH.h"
#include "core/SpectatorGate.h"
#include "core/Allowance.h"   // RuntimeSupported, SeatVerified (the mit-3.7 F1 self-check gate)
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/Sources.h"

#include <intrin.h>   // _ReturnAddress
#include <mutex>
#include <unordered_map>

extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// THE SPECTATOR SEAT (ClickUp 86e3m5feu, 2026-10-12)
//
// THE BUG. MFO's auto-retreat files a ch.9 kIntent_OfferPackage claim (its retreat
// package) and a ch.22 re-entry deny, then posts StopCombat. Field 1006d: the moment the
// follower is out of combat the engine gives it a runtime-created package with procedure
// type 24 (kSpectator; FF001C54 / FF00223A / FF00227D / FF002276 in the deck logs). The
// follower stands and watches the fight it just left, keeps taking hits and drifts, and
// the retreat ends only at MFO's 30 s timeout. The 0x49 seat (core/PackageGate.cpp) never
// sees it: a Spectator package is not an alias-package answer, it is PUT on the actor.
//
// HOW THE ENGINE ASSIGNS IT (disassembled 2026-10-12 on all three builds):
//   1. A detection reaction (AE 0x70B6F0 / call at 0x70B911) between an actor and another
//      it perceives asks the spectate predicate (AE 0x70AF90: the other is not the player,
//      the actor is NOT in combat (slot 0xE3), the other IS in combat, ...). A follower
//      that StopCombat just took out of a live fight answers yes on the next reaction.
//   2. The spectator dispatcher (AE 0x83D5F0 / SE 0x7A62E0 / 1.7.104 0x8524D0) reads the
//      actor's current package (process +0x20, procedure byte +0x24). If it is not already
//      a Spectator package (0x18), it makes the ONE virtual call
//      `actor->InitiateSpectator(target)` = Character vtable slot 0xDC (call [rax+0x6E0] at
//      AE 0x83D631 / SE 0x7A6321 / 1.7.104 0x852511), then adds the target to the running
//      Spectator package if there is one.
//   3. Character::InitiateSpectator (slot 0xDC: AE 0x7270E0 / SE 0x6932D0 / 1.7.104
//      0x739C10; 1.7.104 instruction-identical to 1.6.1170) re-checks its own early outs
//      (in combat, fleeing, ...), allocates 0x120 bytes, constructs a SpectatorPackage
//      (AE 0x7ABDE0) and installs it with PutCreatedPackage (slot 0xDF, [rax+0x6F8]).
//   Completeness: slot 0xDC has exactly that one virtual caller on each build and no direct
//   caller. The SpectatorPackage constructor has two callers: InitiateSpectator and the
//   package-by-type factory (AE 0x48F560), whose callers all pass constant types other than
//   0x18 except the save loader (type byte read from the save stream). So in live play a
//   Spectator package exists ONLY through slot 0xDC.
//
// THE SEAT. write_vfunc on VTABLE_Character[0] slot 0xDC (never PlayerCharacter; the
// player is never spectating through this path). Denying here means the package is never
// allocated: nothing to free, no OnPackageChange, no side effect of the original at all.
// (0xDF, PutCreatedPackage, was rejected as the seat: the package is already allocated and
// owned by the call, so refusing it there leaks or double-frees.)
//
// THE DECISION (the controller's priority rule, core/Sources.h). The actor holds a winning
// ch.9 kIntent_OfferPackage claim naming a package that resolves (the same test the 0x49
// thunk makes before it redirects) AND the claim's priority on the Package facet outranks
// the Engine's: the call is not forwarded and the claimed package keeps the facet. Any
// other actor, an unresolved offer, or an Engine priority >= the claim's: the original
// runs, unchanged. Every call for a claimed actor is a hit on the Engine source (Package
// facet), overruled or stood. For ch.19's internal offer (no client filed it) the priority
// is the winning travel claim's, exactly as core/PackageSources.cpp computes it.
//
// MODES ([PackageGate] iSpectatorSeat in Data/SKSE/Plugins/APMF.ini): 0 = not installed;
// 1 = OBSERVE ONLY (decide, count, log "would deny", always forward); 2 = ENFORCE
// (default). The observation lines are the passive probe (principle 5): the first line per
// actor and claim names the engine call site (return RVA, expected AE 0x83D637 / SE
// 0x7A6327 / 1.7.104 0x852517) so a field log proves the seat runs where the disassembly
// says it does.
//
// THREADING. The thunk runs wherever the engine runs the detection reaction (AI update
// jobs, not necessarily the main thread). It reads only the lock-free RCU ControlMap
// snapshot (TryGetOwningClaimBasis), the lock-free Sources registry and its own atomics;
// the per-actor transition memory takes one small mutex, only for a claimed actor (or to
// erase a stale entry, pre-gated by a relaxed count).
// ============================================================================

namespace apmf::spectatorgate {

    namespace {

        using apmf::log::Hex;
        using apmf::sources::Facet;

        constexpr const char*   kIni          = "Data/SKSE/Plugins/APMF.ini";
        constexpr std::size_t   kSlot         = 0xDC;   // Actor::InitiateSpectator (SE and AE; VR 0xDE, refused)
        constexpr std::uint64_t kHeartbeatMs  = 30000;
        constexpr std::uint64_t kLineCap      = 2000;   // per session; the counters are never capped

        enum Mode : int { kOff = 0, kObserve = 1, kEnforce = 2 };

        std::atomic<bool> g_installed{ false };
        std::atomic<int>  g_mode{ kEnforce };

        // RULE C counters, printed by Heartbeat even at zero.
        std::atomic<std::uint64_t> g_anchor{ 0 };    // every slot-0xDC call, any actor
        std::atomic<std::uint64_t> g_claimed{ 0 };   // the actor held a resolving ch.9 offer
        std::atomic<std::uint64_t> g_denied{ 0 };    // not forwarded (enforce, claim outranks)
        std::atomic<std::uint64_t> g_wouldDeny{ 0 }; // observe mode: would not have been forwarded
        std::atomic<std::uint64_t> g_stood{ 0 };     // the Engine's priority >= the claim's: forwarded
        std::atomic<std::uint64_t> g_lastBeatMs{ 0 };

        // RULE D: one line per (actor, claimed package, decision) transition.
        struct Last {
            RE::FormID claimForm;
            int        decision;   // 0 denied, 1 would deny, 2 stood
            bool operator==(const Last&) const = default;
        };
        std::mutex                           g_mx;
        std::unordered_map<RE::FormID, Last> g_last;
        std::atomic<std::uint32_t>           g_remembered{ 0 };
        std::atomic<std::uint64_t>           g_lines{ 0 };

        // The client priority the ch.9 claim is enforced with on the Package facet: the
        // offer's own client, or, for ch.19's internal offer, the winning travel claim's
        // (core/PackageSources.cpp ClaimPrio, same rule).
        int ClaimPrio(RE::FormID a_actor, APMF_API::Handle a_offer, std::string& a_client) {
            APMF_API::Handle h = a_offer;
            if (apmf::sources::ClientForHandle(h) == apmf::sources::kNoSource) {
                APMF_API::APMF_Param p{};
                float                b  = 0.0f;
                APMF_API::Handle     th = APMF_API::kInvalidHandle;
                if (ControlMap::Get().TryGetOwningClaimBasis(a_actor, APMF_API::kIntent_Travel, p, b, &th)) h = th;
            }
            apmf::sources::SourceId c = apmf::sources::kNoSource;
            const int prio = apmf::sources::ClaimPriority(h, Facet::kPackage, &c);
            a_client = c != apmf::sources::kNoSource ? std::string(apmf::sources::Name(c))
                                                     : std::string("an internal claim");
            return prio;
        }

        void Forget(RE::FormID a_actor) {
            if (g_remembered.load(std::memory_order_relaxed) == 0) return;
            std::scoped_lock lk(g_mx);
            if (g_last.erase(a_actor) != 0) g_remembered.fetch_sub(1, std::memory_order_relaxed);
        }

        void Report(RE::Actor* a_actor, RE::Actor* a_target, RE::FormID a_claimForm, APMF_API::Handle a_h,
                    int a_decision, int a_claimPrio, int a_enginePrio, const std::string& a_client,
                    std::uintptr_t a_ret) {
            const RE::FormID id = a_actor->GetFormID();
            {
                const Last now{ a_claimForm, a_decision };
                std::scoped_lock lk(g_mx);
                auto [it, inserted] = g_last.try_emplace(id, now);
                if (inserted) g_remembered.fetch_add(1, std::memory_order_relaxed);
                else if (it->second == now) return;
                else it->second = now;
            }
            const auto prior = g_lines.fetch_add(1, std::memory_order_relaxed);
            if (prior == kLineCap)
                spdlog::warn("[spectator] SESSION LINE CAP ({}) REACHED -- further [spectator] transition lines are "
                             "suppressed. The heartbeat counters are not capped; do not read the silence as 'no "
                             "Spectator calls'.",
                             kLineCap);
            if (prior >= kLineCap) return;
            const auto  base = REL::Module::get().base();
            const char* name = a_actor->GetName();
            const char* tname = a_target ? a_target->GetName() : nullptr;
            static constexpr const char* kWhat[] = {
                "DENIED: the Spectator package is not created, the claimed package keeps the Package facet",
                "WOULD DENY (observe only, [PackageGate] iSpectatorSeat=1): forwarded, the engine installs its Spectator package",
                "STANDS: forwarded, the engine installs its Spectator package (APMF_Sources.ini)",
            };
            spdlog::info("[spectator] 0x{} '{}' Engine InitiateSpectator (watch 0x{} '{}', engine call site RVA 0x{:X}) "
                         "while ch.9 h={} offers 0x{}. Claim {} priority {} {} Engine priority {} on the Package "
                         "facet -- {}.",
                         Hex(id), name ? name : "?", Hex(a_target ? a_target->GetFormID() : 0), tname ? tname : "?",
                         a_ret >= base ? a_ret - base : a_ret, a_h, Hex(a_claimForm), a_client, a_claimPrio,
                         a_decision == 2 ? "does not outrank" : "outranks", a_enginePrio, kWhat[a_decision]);
        }

        struct InitiateSpectatorHook {
            static void thunk(RE::Actor* a_this, RE::Actor* a_target) {
                g_anchor.fetch_add(1, std::memory_order_relaxed);
                if (!a_this || ControlMap::Get().ControlledCount() == 0) return func(a_this, a_target);

                const RE::FormID     id = a_this->GetFormID();
                APMF_API::APMF_Param claim{};
                float                basis = 0.0f;
                APMF_API::Handle     h     = APMF_API::kInvalidHandle;
                if (!ControlMap::Get().TryGetOwningClaimBasis(id, APMF_API::kIntent_OfferPackage, claim, basis, &h) ||
                    claim.form == 0 || !RE::TESForm::LookupByID<RE::TESPackage>(claim.form)) {
                    // No claim that the 0x49 seat would deliver: the engine's own answer, untouched.
                    Forget(id);
                    return func(a_this, a_target);
                }

                g_claimed.fetch_add(1, std::memory_order_relaxed);
                apmf::sources::NoteHit(apmf::sources::kEngineSource, Facet::kPackage);
                std::string client;
                const int  claimPrio  = ClaimPrio(id, h, client);
                const int  enginePrio = apmf::sources::Priority(apmf::sources::kEngineSource, Facet::kPackage);
                const bool outranks   = claimPrio > enginePrio;
                const bool enforce    = g_mode.load(std::memory_order_relaxed) == kEnforce;
                int decision;
                if (!outranks) {
                    decision = 2;
                    g_stood.fetch_add(1, std::memory_order_relaxed);
                    apmf::sources::NoteStood(apmf::sources::kEngineSource, Facet::kPackage);
                } else if (enforce) {
                    decision = 0;
                    g_denied.fetch_add(1, std::memory_order_relaxed);
                    apmf::sources::NoteOverruled(apmf::sources::kEngineSource, Facet::kPackage);
                } else {
                    decision = 1;
                    g_wouldDeny.fetch_add(1, std::memory_order_relaxed);
                }
                try {
                    Report(a_this, a_target, claim.form, h, decision, claimPrio, enginePrio, client,
                           reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
                } catch (...) {
                }
                if (decision == 0) return;   // DENY: the original never runs, nothing is allocated
                func(a_this, a_target);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

    }

    void Install() {
        if (g_installed.load(std::memory_order_relaxed)) return;
        const int mode = static_cast<int>(GetPrivateProfileIntA("PackageGate", "iSpectatorSeat", kEnforce, kIni));
        const char* why = nullptr;
        if (REL::Module::IsVR()) {
            why = "VR runtime (slot 0xDC is verified on 1.6.1170, 1.5.97 and 1.7.104 only; VR's is 0xDE)";
        } else if (!apmf::allowance::RuntimeSupported()) {
            why = "runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104 (the seat is verified on those three only)";
        } else if (mode == kOff) {
            why = "[PackageGate] iSpectatorSeat=0 in Data/SKSE/Plugins/APMF.ini";
        } else if (mode != kObserve && mode != kEnforce) {
            why = "[PackageGate] iSpectatorSeat is not 0, 1 or 2";
        }
        if (why) {
            spdlog::warn("[spectator] Spectator seat NOT installed -- {}. The engine's Spectator package is NOT held "
                         "off for actors with a ch.9 package claim (the known retreat hole, ClickUp 86e3m5feu).",
                         why);
            return;
        }

        REL::Relocation<std::uintptr_t> vtChar{ RE::VTABLE_Character[0] };
        if (!apmf::allowance::SeatVerified(vtChar.address(), "SpectatorGate.Character.InitiateSpectator(0xDC)")) {
            spdlog::error("[spectator] Spectator seat NOT installed (the self-check refused the Character vtable). "
                          "The engine's Spectator package is NOT held off for ch.9 package claims.");
            return;
        }
        g_mode.store(mode, std::memory_order_relaxed);
        InitiateSpectatorHook::func = vtChar.write_vfunc(kSlot, InitiateSpectatorHook::thunk);
        apmf::sources::NoteVfunc(Facet::kPackage, "SpectatorGate.Character.InitiateSpectator", vtChar.address(), kSlot,
                                 SKSE::stl::unrestricted_cast<std::uintptr_t>(&InitiateSpectatorHook::thunk),
                                 InitiateSpectatorHook::func.address());
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[spectator] Spectator seat installed: Character::InitiateSpectator (slot 0x{:X}), mode {}. An "
                     "actor with a winning ch.9 package claim that outranks the Engine on the Package facet {} the "
                     "engine's Spectator package.",
                     kSlot, mode == kEnforce ? "ENFORCE" : "OBSERVE ONLY",
                     mode == kEnforce ? "does not get" : "is only logged for (nothing denied)");
    }

    void Heartbeat() {
        if (!g_installed.load(std::memory_order_relaxed)) return;
        const auto now = apmf::clock::MonotonicMs();
        if (now - g_lastBeatMs.load(std::memory_order_relaxed) < kHeartbeatMs) return;
        g_lastBeatMs.store(now, std::memory_order_relaxed);   // main-thread-only writer
        spdlog::info("[spectator] H mode={} anchorCalls={} claimedCalls={} denied={} wouldDeny={} engineStood={}",
                     g_mode.load(std::memory_order_relaxed) == kEnforce ? "enforce" : "observe",
                     g_anchor.load(std::memory_order_relaxed), g_claimed.load(std::memory_order_relaxed),
                     g_denied.load(std::memory_order_relaxed), g_wouldDeny.load(std::memory_order_relaxed),
                     g_stood.load(std::memory_order_relaxed));
    }

    void ResetAll() {
        std::scoped_lock lk(g_mx);
        g_last.clear();
        g_remembered.store(0, std::memory_order_relaxed);
    }

}
