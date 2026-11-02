#include "PCH.h"
#include "core/SpectatorGate.h"
#include "core/Allowance.h"   // RuntimeSupported, SeatVerified (the mit-3.7 F1 self-check gate)
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/Sources.h"
#include "channels/CombatSubstrate.h"   // ForWatchTarget (lock-free): the substrate target

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
// THE SUBSTRATE TARGET (marth 2026-11-01, "Extend SpectatorGate"; field: every townsperson reacted
// as if combat was occurring while a follower cast out of combat). The spectate predicate's "the
// other IS in combat" is AE 0x6B6DF0: IsInCombat (slot 0xE3: Actor+0x160 controller with +0x43
// clear) AND NOT the process ignoring-combat bit (AIProcess+0x139). A combat substrate holder
// answers yes whenever the engine has reset that bit (every window, measured). So every high-
// process actor that perceives the holder is handed a Spectator package watching it, and AddThreat
// (AE 0x7ACA70) records the pair (holder, holder+0x104 currentCombatTarget = the anchor). The same
// seat now also refuses InitiateSpectator(observer, target) when the TARGET is a HELD substrate
// holder or the anchor (combatsubstrate::ForWatchTarget). A REAL holder (its real-combat trigger
// fired) is watched like any fight. The deny is enforced FOR the substrate's claim: its client's
// priority on the Package facet against the Engine's, live per call, the same compare as the ch.9
// path (the anchor ranks as the claim of the holder it stands at, a dry run as the client
// default). A held holder as the OBSERVER is never touched here: the predicate and
// InitiateSpectator's own early out (AE 0x727181, slot 0xE3) refuse an observer that is in combat.
// One line per (observer, target) pair per kPairLogMs. The counters are never capped.
//
// THREADING. The thunk runs wherever the engine runs the detection reaction (AI update
// jobs, not necessarily the main thread. The dispatcher's own CombatManager step, AE 0x83E2A0,
// takes CombatManager's lock at +0x18). The substrate target's reads are ANY THREAD: ForWatchTarget
// is the substrate's lock-free slot table (the GetSubstrateState read), ClaimPriority / Priority
// are the Sources registry's any-thread reads. It reads only the lock-free RCU ControlMap
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
        std::atomic<const char*> g_notEnforcing{ "before kDataLoaded (the seat installs there)" };

        // RULE C counters, printed by Heartbeat even at zero.
        std::atomic<std::uint64_t> g_anchor{ 0 };    // every slot-0xDC call, any actor
        std::atomic<std::uint64_t> g_claimed{ 0 };   // the actor held a resolving ch.9 offer
        std::atomic<std::uint64_t> g_denied{ 0 };    // not forwarded (enforce, claim outranks)
        std::atomic<std::uint64_t> g_wouldDeny{ 0 }; // observe mode: would not have been forwarded
        std::atomic<std::uint64_t> g_stood{ 0 };     // the Engine's priority >= the claim's: forwarded
        std::atomic<std::uint64_t> g_lastBeatMs{ 0 };
        // The substrate target (a HELD holder or the anchor watched): RULE C counters.
        std::atomic<std::uint64_t> g_subCalls{ 0 };     // the target was a held holder or the anchor
        std::atomic<std::uint64_t> g_subDenied{ 0 };    // not forwarded (enforce, the substrate's claim outranks)
        std::atomic<std::uint64_t> g_subWouldDeny{ 0 }; // observe mode: would not have been forwarded
        std::atomic<std::uint64_t> g_subStood{ 0 };     // the Engine's priority >= the claim's: forwarded (a HOLE)

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

        // The substrate target's lines: one per (observer, target) pair per kPairLogMs. LEAF lock (no engine call,
        // no spdlog under it). Bounded: at kPairCap the memory is dropped (a pair may then log once more).
        constexpr std::uint64_t                          kPairLogMs = 60000;
        constexpr std::size_t                            kPairCap   = 4096;
        std::mutex                                       g_pairMx;
        std::unordered_map<std::uint64_t, std::uint64_t> g_pairLast;

        bool PairDue(RE::FormID a_observer, RE::FormID a_target, std::uint64_t a_now) {
            const std::uint64_t key = (static_cast<std::uint64_t>(a_observer) << 32) | a_target;
            std::scoped_lock    lk(g_pairMx);
            if (g_pairLast.size() >= kPairCap) g_pairLast.clear();
            auto [it, inserted] = g_pairLast.try_emplace(key, a_now);
            if (inserted) return true;
            if (a_now - it->second < kPairLogMs) return false;
            it->second = a_now;
            return true;
        }

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
            spdlog::info("[spectator] 0x{} '{}' Engine InitiateSpectator (watch 0x{} '{}', engine call site RVA 0x{}) "
                         "while ch.9 h={} offers 0x{}. Claim {} priority {} {} Engine priority {} on the Package "
                         "facet -- {}.",
                         Hex(id), name ? name : "?", Hex(a_target ? a_target->GetFormID() : 0), tname ? tname : "?",
                         Hex(a_ret >= base ? a_ret - base : a_ret, 1), a_h, Hex(a_claimForm), a_client, a_claimPrio,
                         a_decision == 2 ? "does not outrank" : "outranks", a_enginePrio, kWhat[a_decision]);
        }

        // The substrate target. True = DENY (the original must not run). ANY THREAD.
        bool SubstrateTarget(RE::Actor* a_this, RE::Actor* a_target, apmf::combatsubstrate::WatchTarget a_kind,
                             APMF_API::Handle a_claim, std::uintptr_t a_ret) {
            g_subCalls.fetch_add(1, std::memory_order_relaxed);
            apmf::sources::NoteHit(apmf::sources::kEngineSource, Facet::kPackage);
            apmf::sources::SourceId c = apmf::sources::kNoSource;
            const int  claimPrio  = apmf::sources::ClaimPriority(a_claim, Facet::kPackage, &c);
            const int  enginePrio = apmf::sources::Priority(apmf::sources::kEngineSource, Facet::kPackage);
            const bool outranks   = claimPrio > enginePrio;
            const bool enforce    = g_mode.load(std::memory_order_relaxed) == kEnforce;
            int decision;
            if (!outranks) {
                decision = 2;
                g_subStood.fetch_add(1, std::memory_order_relaxed);
                apmf::sources::NoteStood(apmf::sources::kEngineSource, Facet::kPackage);
            } else if (enforce) {
                decision = 0;
                g_subDenied.fetch_add(1, std::memory_order_relaxed);
                apmf::sources::NoteOverruled(apmf::sources::kEngineSource, Facet::kPackage);
            } else {
                decision = 1;
                g_subWouldDeny.fetch_add(1, std::memory_order_relaxed);
            }
            try {
                const RE::FormID oid = a_this->GetFormID();
                const RE::FormID tid = a_target->GetFormID();
                if (PairDue(oid, tid, apmf::clock::MonotonicMs())) {
                    const auto prior = g_lines.fetch_add(1, std::memory_order_relaxed);
                    if (prior == kLineCap)
                        spdlog::warn("[spectator] SESSION LINE CAP ({}) REACHED -- further [spectator] lines are "
                                     "suppressed. The heartbeat counters are not capped; do not read the silence as "
                                     "'no Spectator calls'.",
                                     kLineCap);
                    if (prior < kLineCap) {
                        const auto        base  = REL::Module::get().base();
                        const char*       name  = a_this->GetName();
                        const char*       tname = a_target->GetName();
                        const char*       kind  = a_kind == apmf::combatsubstrate::WatchTarget::kAnchor
                                                      ? "the substrate anchor"
                                                      : "a HELD substrate holder";
                        const std::string client = c != apmf::sources::kNoSource ? std::string(apmf::sources::Name(c))
                                                                                 : std::string("the client default");
                        static constexpr const char* kWhat[] = {
                            "spectator of a held substrate refused: the Spectator package is not created",
                            "spectator of a held substrate WOULD be refused (observe only, [PackageGate] "
                            "iSpectatorSeat=1): forwarded, the engine installs its Spectator package",
                            "spectator of a held substrate STANDS: forwarded, the engine installs its Spectator "
                            "package (APMF_Sources.ini: the town-panic facet is OPEN for this pair)",
                        };
                        spdlog::log(decision == 0 ? spdlog::level::info : spdlog::level::warn,
                                    "[spectator] 0x{} '{}' Engine InitiateSpectator watching 0x{} '{}' ({}, engine call "
                                    "site RVA 0x{}). Substrate claim h={} ({}) priority {} {} Engine priority {} on the "
                                    "Package facet -- {}. (one line per pair per {} s)",
                                    Hex(oid), name ? name : "?", Hex(tid), tname ? tname : "?", kind,
                                    Hex(a_ret >= base ? a_ret - base : a_ret, 1), a_claim, client, claimPrio,
                                    outranks ? "outranks" : "does not outrank", enginePrio, kWhat[decision],
                                    kPairLogMs / 1000);
                    }
                }
            } catch (...) {
            }
            return decision == 0;
        }

        struct InitiateSpectatorHook {
            static void thunk(RE::Actor* a_this, RE::Actor* a_target) {
                g_anchor.fetch_add(1, std::memory_order_relaxed);
                if (a_this && a_target) {   // the substrate target first: a held holder or the anchor is never watched
                    APMF_API::Handle sh   = APMF_API::kInvalidHandle;
                    const auto       kind = apmf::combatsubstrate::ForWatchTarget(a_target->GetFormID(), sh);
                    if (kind != apmf::combatsubstrate::WatchTarget::kNone &&
                        SubstrateTarget(a_this, a_target, kind, sh, reinterpret_cast<std::uintptr_t>(_ReturnAddress())))
                        return;   // DENY: the original never runs, nothing is allocated
                }
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
            g_notEnforcing.store(why, std::memory_order_release);
            spdlog::warn("[spectator] Spectator seat NOT installed -- {}. The engine's Spectator package is NOT held "
                         "off for actors with a ch.9 package claim (the known retreat hole, ClickUp 86e3m5feu), nor for "
                         "actors that watch a held combat substrate holder (the town-panic facet).",
                         why);
            return;
        }

        REL::Relocation<std::uintptr_t> vtChar{ RE::VTABLE_Character[0] };
        if (!apmf::allowance::SeatVerified(vtChar.address(), "SpectatorGate.Character.InitiateSpectator(0xDC)")) {
            g_notEnforcing.store("the self-check refused the Character vtable", std::memory_order_release);
            spdlog::error("[spectator] Spectator seat NOT installed (the self-check refused the Character vtable). "
                          "The engine's Spectator package is NOT held off for ch.9 package claims, nor for watchers of a "
                          "held combat substrate holder.");
            return;
        }
        g_mode.store(mode, std::memory_order_relaxed);
        InitiateSpectatorHook::func = vtChar.write_vfunc(kSlot, InitiateSpectatorHook::thunk);
        apmf::sources::NoteVfunc(Facet::kPackage, "SpectatorGate.Character.InitiateSpectator", vtChar.address(), kSlot,
                                 SKSE::stl::unrestricted_cast<std::uintptr_t>(&InitiateSpectatorHook::thunk),
                                 InitiateSpectatorHook::func.address());
        g_installed.store(true, std::memory_order_release);
        g_notEnforcing.store(mode == kEnforce ? "" : "[PackageGate] iSpectatorSeat=1 (observe only)",
                             std::memory_order_release);
        spdlog::info("[spectator] Spectator seat installed: Character::InitiateSpectator (slot 0x{}), mode {}. An "
                     "actor with a winning ch.9 package claim that outranks the Engine on the Package facet {} the "
                     "engine's Spectator package. An actor that would watch a HELD combat substrate holder or the anchor "
                     "(the substrate's claim outranking the Engine there) is treated the same.",
                     Hex(kSlot, 2), mode == kEnforce ? "ENFORCE" : "OBSERVE ONLY",
                     mode == kEnforce ? "does not get" : "is only logged for (nothing denied: observe only)");
    }

    bool Enforcing() {
        return g_installed.load(std::memory_order_acquire) && g_mode.load(std::memory_order_relaxed) == kEnforce;
    }

    const char* NotEnforcingReason() {
        const char* r = g_notEnforcing.load(std::memory_order_acquire);
        return r ? r : "unknown";
    }

    void Heartbeat() {
        if (!g_installed.load(std::memory_order_relaxed)) return;
        const auto now = apmf::clock::MonotonicMs();
        if (now - g_lastBeatMs.load(std::memory_order_relaxed) < kHeartbeatMs) return;
        g_lastBeatMs.store(now, std::memory_order_relaxed);   // main-thread-only writer
        spdlog::info("[spectator] H mode={} anchorCalls={} claimedCalls={} denied={} wouldDeny={} engineStood={} "
                     "substrateTargetCalls={} substrateDenied={} substrateWouldDeny={} substrateEngineStood={}",
                     g_mode.load(std::memory_order_relaxed) == kEnforce ? "enforce" : "observe",
                     g_anchor.load(std::memory_order_relaxed), g_claimed.load(std::memory_order_relaxed),
                     g_denied.load(std::memory_order_relaxed), g_wouldDeny.load(std::memory_order_relaxed),
                     g_stood.load(std::memory_order_relaxed), g_subCalls.load(std::memory_order_relaxed),
                     g_subDenied.load(std::memory_order_relaxed), g_subWouldDeny.load(std::memory_order_relaxed),
                     g_subStood.load(std::memory_order_relaxed));
    }

    void ResetAll() {
        {
            std::scoped_lock lk(g_mx);
            g_last.clear();
            g_remembered.store(0, std::memory_order_relaxed);
        }
        std::scoped_lock lk(g_pairMx);
        g_pairLast.clear();
    }

}
