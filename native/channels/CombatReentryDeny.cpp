#include "PCH.h"
#include "core/Sources.h"   // Harbinger 1a: seat census
#include "core/FacetAttribution.h"   // Harbinger 1b: the StartCombat caller + the priority decision
#include "channels/CombatReentryDeny.h"
#include "channels/AnchorGuard.h"       // the null-target group-join refusal (SEV-1 2026-10-19) rides this seat
#include "channels/CombatSubstrate.h"   // the anchor-target refusal (S2a) rides this seat
#include "core/Allowance.h"   // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/MainThread.h"   // Post: Settle runs right after Drain publishes
#include "core/Registry.h"

#include <cmath>
#include <intrin.h>   // _ReturnAddress (core/EquipSink.cpp / core/EquipGate.cpp precedent)
#include <mutex>
#include <shared_mutex>

// Win32 INI reader, declared by hand (the PCH does not pull in <Windows.h>) -- the same
// one-line import channels/TargetPin.cpp and channels/CombatEntry.cpp use.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* lpAppName, const char* lpKeyName, int nDefault, const char* lpFileName);

// ============================================================================
// Channel 22 -- COMBAT RE-ENTRY DENY (kIntent_CombatReentryDeny, ABI v15, ClickUp
// 86e3ex5v9). From MFO's confidence / leash assessment: a retreating follower is pulled
// back into combat by the engine on every tick, so the client has to repeat StopCombat.
// With this claim held the client calls StopCombat ONCE and the engine cannot put the
// actor back into combat until the window ends.
//
// WHAT IT IS. A client claims {actor, window seconds} (param.fval; 0 = 10 s, at most
// 120 s). While that is the WINNING kIntent_CombatReentryDeny claim on the actor and its
// window is running, EVERY engine StartCombat for that actor is REFUSED at its source -- a new
// entry, and (review F2, ruled option (a)) a target-add while the actor is still fighting.
// Only ch.21's own entry passes. Nothing else changes: no StopCombat, no engine state written,
// no other facet touched.
//
// HARBINGER 1b (2026-10-12): THE CALLER IS NAMED AND RANKED. At the seat, the frame after
// StartCombat's own self-check return site is StartCombat's CALLER (core/FacetAttribution
// CallerBelow, a stack scan; no new address). It resolves to Engine (SkyrimSE.exe), DirectCalls
// (skse64) or the calling DLL as a plugin source, and the refusal stands ONLY when the claim's
// priority on the Combat facet is strictly above that source's (APMF_Sources.ini; with the
// defaults, client 50 > unregistered 20, every engine / other-DLL entry is refused exactly as
// before). A caller that does not rank below the claim -- including the claiming client's own DLL
// calling StartCombat itself -- PASSES, counted as sourcePass and logged; a caller the seat cannot
// name is the degrade rule (allowed + alerted, or ranked at the Unattributed row with
// [UnknownSources] Combat=override). KNOWN LIMIT: a Papyrus Actor.StartCombat runs inside
// SkyrimSE.exe, so it is named Engine, not DirectCalls (telling them apart needs per-runtime rows
// for the ten StartCombat callers -- a later slice). The same seat also names any StartCombat on
// an actor holding a ch.21 entry claim (ObserveEntryClaim): attribution only, it stands.
//
// HOW AN ACTOR ENTERS COMBAT (disassembly of both unpacked images, 2026-09-25; scratchpad
// log apmf-reentry.md has the listings).
//   An actor is in combat exactly when it has a CombatController (Actor +0x160 AE / +0x158
//   SE). A controller is built in ONE place outside save loading: CombatManager AE 46873 /
//   46874 (SE 45573 / 45574), and each of those has callers ONLY inside Actor::StartCombat
//   (AE id 38561 at 0x6B6930 / SE id 37608 at 0x6251B0: AE 0x6B6CB7, 0x6B6CD0, 0x6B6D78). The
//   controller constructor (AE 33214 / SE 32467) has one other caller, AE 37650 / SE 36642,
//   the actor's save-LOAD path (it rebuilds the saved controller from the save buffer); claims
//   never cross a load. So EVERY combat entry the engine makes for an actor -- detection,
//   being attacked, an ally's fight, a group join (r8 != 0), a script's StartCombat, another
//   mod's call -- is `Actor::StartCombat(this = that actor, ...)`. Ten direct callers per
//   runtime (AE 37289, 37496 x2, 39212 (tail jmp), 39652 x2, 39695, 40079, 40814, 41407;
//   SE 36299, 36496 x2, 38252, 38621 x2, 38667, 39012, 39712, 40393), no data reference to
//   it. Every caller tolerates a false return (40814 tests it and takes its failure branch;
//   the others ignore it and do not touch the controller after the call).
//   StartCombat is NOT virtual. Patching its entry or its ten callers is a call-site patch
//   on a public engine function (INVARIANTS #17 forbids it; #17a does not apply).
//
// THE SEAT. StartCombat's refusal checks run FIRST, before its global spinlock, before its
// re-arm equip (38893 / 37937) and before anything else it does. The fourth of them is a
// VIRTUAL call on the actor itself:
//     AE 0x6B69BF: B2 01          mov  dl, 1
//             +02: 48 8B CF       mov  rcx, rdi          (rdi = this)
//             +05: FF 90 C8 04 00 00  call [rax+0x4C8]   (Character slot 0x99, IsDead(true))
//     AE 0x6B69CA: 84 C0 / 0F 85  test al, al; jne -> the refusal epilogue (0x6B6D9B)
//   SE: the same eleven bytes at 0x62523D, return address 0x625248, jne 0x625623.
//   The refusal epilogue clears the result byte, restores the thread's TLS context word and
//   returns false: NOTHING has been done yet. The slot's function is Actor::IsDead(bool)
//   (AE 0x674ED0 id 37483 / SE 0x5E3160 id 36484: rcx this, dl bool, al result, a leaf).
//   write_vfunc on Character slot 0x99, chaining. The original ALWAYS runs first. Its answer
//   is changed ONLY when the thunk's own return address is exactly StartCombat's self-check
//   return site (verified row + 11 bytes), and then only from "not dead" to "dead" -- i.e.
//   StartCombat's own "enter" is turned into its own "refuse", through a check the engine
//   already makes there -- when ALL of these hold:
//     * the actor holds the WINNING kIntent_CombatReentryDeny claim (published RCU read);
//     * its window has not elapsed (the window runs from the claim's OWN request / last Repoint
//       time, review F3: a claim taking over from a rival gets only what is left of it);
//     * the call is not ch.21's own entry for this actor (ClientEntryScope, below).
//   The seat does NOT look at whether the actor is in combat (review F2, option (a)): an actor
//   still fighting is not stopped (the client ends that fight, Actor::StopCombat, its call), but
//   StartCombat's in-combat path -- adding a target to its group -- is refused too. That removed
//   the one read the seat made of actor state (the controller pointer), and with it the race
//   against a StopCombat on another thread between that read and StartCombat's own lock.
//   Every other caller of IsDead -- thousands per frame -- gets the original answer: the
//   return-address compare is the first thing the thunk does after the original.
//
// ch.21 PRECEDENCE (documented, INVARIANTS #0 (h) condition 4). A kIntent_CombatEntry claim is
// a client's declared decision to put this actor into combat; channels/CombatEntry.cpp holds a
// ClientEntryScope around its one StartCombat call and the seat lets that call through. It
// does not end the deny: once that fight ends, the window (if still running) refuses the
// engine's re-entries again.
//
// PRINCIPLE 5 (a seat must be OBSERVED running before anything relies on it). The seat counts
// every StartCombat self-check it sees, claim or no claim, and logs the FIRST one of the
// session ("seat OBSERVED"). Poll() carries a DENY-MISS detector: a denied actor that GAINS a
// combat controller while its window runs without a ch.21 entry (or, 1b, a source the claim does
// not outrank) passing the seat is logged loudly (principle 7). A known way to miss: another DLL wraps Character slot 0x99 AFTER
// Harbinger with a thunk that CALLS (not tail-jumps to) the previous entry; StartCombat's return
// address is then theirs, never ours. Every Engage / Repoint line warns when the slot no longer
// holds this thunk. The detector samples every 250 ms, so a StopCombat and a new entry inside
// one sample are not seen as a transition; that is a limit of the detector, not of the deny.
//
// THE CLAIM ENDS, AND HARBINGER RELEASES IT ITSELF (logged "deny ended: <reason>", repeated on
// the Release line), when the window elapses or the owner dies -- Poll(), or Settle() right
// after a takeover whose own window is already over. A client's Release, an outranking claim,
// a save load / new game (claims are never saved; ResetAll) and the actor unloading also end
// it. Repoint(handle, &param) restarts that claim's window from the moment it was called.
// Nothing is undone on any end: nothing was written.
//
// WHOSE TIME (review F3, the ch.21 F1 "no stale declaration" line). ControlMap tells this file
// when each kIntent_CombatReentryDeny claim was REQUESTED or last REPOINTED (NoteRequest /
// NoteRepoint, any thread, keyed by handle). Apply() cannot know the winning handle inside
// Drain, so it sets a provisional deadline (now + window) and posts Settle(), which runs from the
// main-thread pump right after Drain publishes: it reads the WINNING handle and sets the
// deadline to that claim's own time + its window. A claim that took over after its own window
// ran out is ended there with "window elapsed". The provisional deadline stands for at most
// that one pump.
//
// DENY-COMPLETENESS (principle 2, 2026-09-25 scope: the client owns the consequences). The
// facet is "this actor ENTERS combat". Competing sources: every engine / script / mod
// StartCombat on this actor -- DENIED at the one choke point (above); the save-load rebuild --
// out of scope (no claim crosses a load). NOT denied, by design: other actors fighting THIS
// actor (their StartCombat has them as `this`; hits, spells and pursuit are their facets); a
// fight the actor is already in (it is not stopped, only kept from growing). Harbinger
// switches no facet on.
//
// THREADING. StartCombat runs on whatever thread asks (the main thread, BSJobs combat /
// detection jobs, the script VM). The seat reads the ControlMap's published snapshot
// (lock-free) and this file's map under a shared_lock (written on the game thread under a
// unique_lock). It reads no actor state beyond its FormID, dereferences nothing of the
// engine's and takes no engine lock. The engine's own spinlock is not yet held at the seat.
// The request-time map (g_reqs) is written from any client thread and read on the game thread,
// under its own mutex.
//
// THE ANCHOR-TARGET REFUSAL (combat substrate S2a, INVARIANTS #0 (j); MFO
// Docs/COMBAT-SUBSTRATE-DESIGN.md 3.5). The same slot-0x99 thunk answers a SECOND return site:
// StartCombat's check of its TARGET, `target->IsDead(false)`:
//     AE +0xCC: 48 8B 06 / 33 D2 / 48 8B CE / FF 90 C8 04 00 00   (mov rax,[rsi]; xor edx,edx;
//               mov rcx,rsi; call [rax+0x4C8]) -> return 0x6B6A0A; SE +0xCA, return 0x625288
//   (verified row Substrate.StartCombat.TargetIsDeadCall). There the IsDead `this` is the TARGET,
//   so the actor that called StartCombat is not an argument: it is StartCombat's own `this`, held
//   in rdi (set at AE +0x29 / SE +0x2A, `mov rdi, rcx`, unwritten until after this call; the row
//   at +0x8F proves `mov rcx, rdi` for the self-check). A 17-byte stub sits in the vtable slot in
//   front of the thunk: `mov r8, rdi; jmp [rip+0]; <thunk>` (49 89 F8 FF 25 00 00 00 00 + 8 bytes),
//   so the thunk receives rdi as a third argument. r8 is a volatile argument register that IsDead
//   does not take, so no caller depends on it surviving the call, and the jump (not a call) keeps
//   the return address the compares below read. The third argument is READ only when the return
//   address is the target site; everywhere else it is ignored (it is whatever rdi held). The
//   answer: "dead" (StartCombat refuses before its spinlock, before anything) when the target is
//   the substrate's anchor and the caller is not a live holder still under its substrate
//   (combatsubstrate::RefuseAnchorTarget). Without the target row or the stub this seat is
//   exactly the ch.22 seat it was, and the substrate refuses to install.
//
// THE GROUP-JOIN REFUSAL (SEV-1 2026-10-19, channels/AnchorGuard.cpp). A NULL-target StartCombat
// with a group skips the TARGET check (AE 0x6B69F7 / SE 0x625275 `test rsi,rsi; je`) and joins
// the group, taking its targets: in a substrate group, the anchor. When anchorguard::InstallSeat
// verifies StartCombat's register copies (row AnchorGuard.StartCombat.TargetGroupRegs: AE +0x23 /
// SE +0x24 `mov rbp,r8; mov rsi,rdx; mov rdi,rcx`, nothing writes rsi / rbp / rdi again before the
// self check) the stub is WIDENED to 53 bytes (BuildRdiStub):
//     mov rax, <self-check return address>; cmp [rsp], rax; jne generic
//     mov r8, rsi; mov r9, rbp; jmp [rip+0] <thunk>          (the self site: target, group)
//   generic:
//     mov r8, rdi; jmp [rip+0] <thunk>                       (every other caller, as before)
// rax is volatile and carries no argument at a call (IsDead takes rcx / dl), r9 is a volatile argument
// register IsDead does not take, and every branch is a JUMP, so the return address the compares
// read is still the engine's. At the self site, after ch.22's own answer did not refuse, the thunk
// asks anchorguard::AnswerJoin. 1.7.104 and a refused row keep the 17-byte stub (r8 = rdi only)
// and AnswerJoin is never called.
//
// VERSION ROBUSTNESS. Two self-checked rows derived from our own executables: the Character
// vtable (spec.json "Character", slot 0x99 listed) and the StartCombat self-check call site
// (spec.json "ReentryDeny.StartCombat.SelfIsDeadCall": StartCombat's signature, then the eleven
// bytes at +0x8F AE / +0x8D SE, byte-checked again at runtime by REL::SelfCheck). Exact
// 1.6.1170 / 1.5.97 only, VR refused, [CombatReentryDeny] bCombatReentryDeny. Either row
// refused = nothing installed and every claim refused.
// ============================================================================

namespace {

    using apmf::log::Hex;

    constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

    constexpr float         kDefaultWindowSec = 10.0f;   // param.fval == 0
    constexpr float         kMaxWindowSec     = 120.0f;  // larger values are clamped (logged)
    constexpr std::size_t   kIsDeadSlot       = 0x99;    // Actor::IsDead (Character, both runtimes)
    constexpr std::uint64_t kLogEveryMs       = 5000;    // the rate limit for the miss line

    // StartCombat's self-check call: `mov dl,1; mov rcx,rdi; call [rax+0x4C8]` -- 11 bytes,
    // starting at these offsets from StartCombat (the verified call-site row). The return site
    // is the byte after them.
    constexpr std::uintptr_t kSelfCheckOffsetAE = 0x8F;   // 0x6B69BF - 0x6B6930
    constexpr std::uintptr_t kSelfCheckOffsetSE = 0x8D;   // 0x62523D - 0x6251B0
    constexpr std::uintptr_t kSelfCheckLen      = 11;
    // StartCombat's TARGET check: `mov rax,[rsi]; xor edx,edx; mov rcx,rsi; call [rax+0x4C8]` -- 14
    // bytes (verified row Substrate.StartCombat.TargetIsDeadCall); the return site is after them.
    constexpr std::uintptr_t kTargetCheckOffsetAE = 0xCC;   // 0x6B69FC - 0x6B6930
    constexpr std::uintptr_t kTargetCheckOffsetSE = 0xCA;   // 0x62527A - 0x6251B0
    constexpr std::uintptr_t kTargetCheckLen      = 14;

    std::atomic<bool>        g_installed{ false };
    std::atomic<bool>        g_installTried{ false };
    std::atomic<const char*> g_notInstalledReason{ "before kDataLoaded (the seat installs there)" };

    // Written once at Install, before the vtable write that makes the thunk reachable.
    std::uintptr_t g_siteRet = 0;   // StartCombat's self-check return address
    std::uintptr_t g_vtChar  = 0;   // the Character vtable the thunk sits on
    std::uintptr_t g_targetRet = 0; // StartCombat's TARGET-check return address (0 = not installed)
    std::uintptr_t g_slotFn    = 0; // what the slot holds: the rdi stub, or the thunk itself

    // Principle 5: every StartCombat self-check the seat saw, claim or no claim.
    std::atomic<std::uint64_t> g_siteSeen{ 0 };

    thread_local RE::FormID t_passActor = 0;   // ClientEntryScope: ch.21's own entry, this thread

    // One entry per actor with an engaged ch.22. Written ONLY on the game thread under a
    // unique_lock; read by the seat on any thread under a shared_lock. Everything the seat or
    // Poll mutates is atomic, so a shared_lock is enough for them.
    struct Deny {
        std::uint64_t deadlineMs = 0;   // monotonic ms; the window ends here
        std::uint64_t windowMs   = 0;   // the winning claim's window length
        std::uint32_t gen        = 0;   // bumps per Apply; Settle carries it
        // seat
        mutable std::atomic<std::uint32_t> seatHits{ 0 };      // StartCombat self-checks for this actor
        mutable std::atomic<std::uint32_t> denied{ 0 };        // entries refused
        mutable std::atomic<std::uint32_t> clientPass{ 0 };    // passed: ch.21's own entry
        mutable std::atomic<std::uint32_t> expiredPass{ 0 };   // passed: window over, Poll not yet run
        mutable std::atomic<std::uint32_t> sourcePass{ 0 };    // passed: the caller's source does not rank
                                                               //   below the claim (Harbinger 1b), or unnamed
        mutable std::atomic<bool>          firstDenyLogged{ false };
        // Poll (game thread)
        mutable std::atomic<bool>          hadController{ false };
        mutable std::atomic<std::uint32_t> clientPassSeen{ 0 };
        mutable std::atomic<std::uint32_t> sourcePassSeen{ 0 };
        mutable std::atomic<std::uint32_t> seatHitsSeen{ 0 };
        mutable std::atomic<std::uint32_t> missed{ 0 };
        mutable std::atomic<std::uint64_t> lastMissLogMs{ 0 };
        mutable std::atomic<bool>          ending{ false };
        mutable std::atomic<const char*>   endedReason{ nullptr };
    };

    std::shared_mutex                    g_mx;
    std::unordered_map<RE::FormID, Deny> g_denies;
    // Relaxed pre-gate: with no claim anywhere this one load (after the return-address
    // compare) is the seat's whole cost. Updated under g_mx's unique_lock.
    std::atomic<std::size_t>             g_count{ 0 };
    std::uint32_t                        g_nextGen = 0;   // game thread

    // Review F3: when each claim was requested / last repointed. Keyed by handle; written by
    // NoteRequest / NoteRepoint on the client's thread, read by Settle on the game thread.
    // Dropped per actor when the actor's last claim is released, and at ResetAll.
    struct Req { RE::FormID actor = 0; std::uint64_t atMs = 0; };
    std::mutex                                 g_reqMx;
    std::unordered_map<APMF_API::Handle, Req>  g_reqs;
    std::atomic<std::size_t>                   g_reqCount{ 0 };   // NoteRepoint's pre-gate

    bool HasController(RE::Actor* a) {
        return a->GetActorRuntimeData().combatController != nullptr;   // the POINTER only
    }

    bool RateOk(std::atomic<std::uint64_t>& last) {
        const auto now  = apmf::clock::MonotonicMs();
        auto       prev = last.load(std::memory_order_relaxed);
        return now - prev >= kLogEveryMs && last.compare_exchange_strong(prev, now, std::memory_order_relaxed);
    }

    // Harbinger 1b, ch.21 (attribution only: ch.21 enforces nothing after its one call). An
    // Actor::StartCombat for an actor holding a winning kIntent_CombatEntry claim that is NOT
    // ch.21's own entry is another source deciding the actor's combat entry: named and counted
    // (it stands). An actor that also holds a ch.22 claim is attributed by ch.22's decision
    // instead (one hit per call). Any thread; reads the published claim snapshot only.
    void ObserveEntryClaim(RE::FormID a_self) {
        if (t_passActor == a_self || apmf::ControlMap::Get().ControlledCount() == 0) return;
        APMF_API::APMF_Param entry{};
        if (!apmf::ControlMap::Get().TryGetOwningClaim(a_self, APMF_API::kIntent_CombatEntry, entry)) return;
        APMF_API::APMF_Param deny{};
        if (apmf::ControlMap::Get().TryGetOwningClaim(a_self, APMF_API::kIntent_CombatReentryDeny, deny)) return;
        const auto caller = apmf::facetattr::CallerBelow(g_siteRet);
        if (caller.self) return;   // APMF's own call outside the scope: not an outside source (none exists today)
        apmf::facetattr::Observe(apmf::sources::Facet::kCombat, caller.src, a_self,
                                 caller.found ? "Actor::StartCombat called from an address in no module"
                                              : "Actor::StartCombat's caller frame was not found on the stack");
        if (apmf::facetattr::LineDue(apmf::sources::Facet::kCombat, a_self, 21u << 8 | 1u)) {
            const auto base = REL::Module::get().base();
            spdlog::info("[ch.21] 0x{} StartCombat by {} (caller RVA 0x{}) while a ch.21 entry claim names 0x{}: "
                         "another source decides this actor's combat entry; it stands (ch.21 makes one call per "
                         "declaration and enforces nothing after it).",
                         Hex(a_self),
                         caller.src != apmf::sources::kNoSource ? std::string(apmf::sources::Name(caller.src))
                                                                : std::string("an unattributed caller"),
                         Hex(caller.addr >= base ? caller.addr - base : caller.addr, 1), Hex(entry.form));
        }
    }

    // The seat decision, only for StartCombat's own self-check. Returns the answer IsDead
    // gives StartCombat: `engineDead` unless this call is denied (then true).
    bool AnswerSelfCheck(RE::Actor* a_this, bool engineDead) {
        const auto seen = g_siteSeen.fetch_add(1, std::memory_order_relaxed) + 1;
        if (seen == 1) {
            spdlog::info("[ch.22] seat OBSERVED: Actor::StartCombat's own self-check reached the Character::IsDead "
                         "seat (first time this session, actor 0x{}). The re-entry deny can act.",
                         Hex(a_this ? a_this->GetFormID() : 0));
        }
        if (engineDead || !a_this) return engineDead;              // the engine refuses anyway
        try {
            ObserveEntryClaim(a_this->GetFormID());
        } catch (...) {
        }
        if (g_count.load(std::memory_order_relaxed) == 0) return engineDead;

        const RE::FormID     self = a_this->GetFormID();
        APMF_API::APMF_Param claim{};
        float                basis = 0.0f;
        APMF_API::Handle     h     = APMF_API::kInvalidHandle;
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(self, APMF_API::kIntent_CombatReentryDeny, claim, basis,
                                                             &h))
            return engineDead;

        std::shared_lock lk(g_mx);
        const auto it = g_denies.find(self);
        if (it == g_denies.end()) return engineDead;
        const Deny& d = it->second;
        d.seatHits.fetch_add(1, std::memory_order_relaxed);
        if (d.ending.load(std::memory_order_relaxed)) return engineDead;   // Harbinger is ending it
        if (t_passActor == self) {                                         // ch.21's own entry
            d.clientPass.fetch_add(1, std::memory_order_relaxed);
            return engineDead;
        }
        const auto now = apmf::clock::MonotonicMs();
        if (now >= d.deadlineMs) {                                         // Poll ends the claim
            d.expiredPass.fetch_add(1, std::memory_order_relaxed);
            return engineDead;
        }
        // Harbinger 1b: WHO CALLED StartCombat (the frame after StartCombat's own self-check return
        // site), and whether the claim outranks it on the Combat facet.
        // Nothing may unwind into StartCombat: a failure here is an unattributed caller, allowed.
        apmf::facetattr::Caller   caller;
        apmf::facetattr::Verdict v;
        try {
            caller = apmf::facetattr::CallerBelow(g_siteRet);
            v      = apmf::facetattr::Decide(
                apmf::sources::Facet::kCombat, h, caller.src, self,
                caller.found ? (caller.self ? "Actor::StartCombat called from inside APMF outside ch.21's entry"
                                            : "Actor::StartCombat called from an address in no module")
                             : "Actor::StartCombat's caller frame was not found on the stack");
        } catch (...) {
            d.sourcePass.fetch_add(1, std::memory_order_relaxed);
            return engineDead;
        }
        if (!v.enforce) {
            d.sourcePass.fetch_add(1, std::memory_order_relaxed);
            if (apmf::facetattr::LineDue(apmf::sources::Facet::kCombat, self, 22u << 8 | 1u)) {
                const auto base = REL::Module::get().base();
                spdlog::info("[ch.22] 0x{} StartCombat by {} (caller RVA 0x{}) PASSES the re-entry deny: {}. "
                             "Claim h={} ({} priority {}) does not outrank it (priority {}) on the Combat facet "
                             "(APMF_Sources.ini).",
                             Hex(self), apmf::sources::Name(v.src),
                             Hex(caller.addr >= base ? caller.addr - base : caller.addr, 1),
                             v.unknown ? "the caller could not be named (allowed, alerted)" : "its source stands",
                             h, apmf::facetattr::NameOr(v.client, "an internal claim"), v.claimPrio, v.srcPrio);
            }
            return engineDead;
        }
        d.denied.fetch_add(1, std::memory_order_relaxed);   // DENY: StartCombat refuses, nothing done
        if (!d.firstDenyLogged.exchange(true, std::memory_order_relaxed)) {
            spdlog::info("[ch.22] 0x{} FIRST DENY: {} called Actor::StartCombat for the actor; refused at its own "
                         "self-check, before it did anything (claim {} priority {} outranks it, priority {}). "
                         "{:.1f} s of the window left.",
                         Hex(self), apmf::sources::Name(v.src), apmf::facetattr::NameOr(v.client, "an internal claim"),
                         v.claimPrio, v.srcPrio, static_cast<double>(d.deadlineMs - now) / 1000.0);
        } else if (apmf::facetattr::LineDue(apmf::sources::Facet::kCombat, self, 22u << 8 | 2u)) {
            spdlog::info("[ch.22] 0x{} captured {} overruled: its StartCombat was refused (claim {} priority {} > {}). "
                         "({} refused so far)",
                         Hex(self), apmf::sources::Name(v.src), apmf::facetattr::NameOr(v.client, "an internal claim"),
                         v.claimPrio, v.srcPrio, d.denied.load(std::memory_order_relaxed));
        }
        return true;
    }

    // The combat substrate's anchor refusal at StartCombat's TARGET check (see the header block).
    // `a_this` is the target; `a_caller` is StartCombat's `this` (rdi), valid at this site only.
    bool AnswerTargetCheck(RE::Actor* a_this, RE::Actor* a_caller, bool engineDead) {
        if (engineDead || !a_this) return engineDead;
        try {
            return apmf::combatsubstrate::RefuseAnchorTarget(a_this, a_caller);
        } catch (...) {
            return engineDead;   // nothing may unwind into StartCombat
        }
    }

    // The widened stub is in the slot (the self site hands rsi / rbp in r8 / r9). Written once at Install,
    // before the vtable write.
    bool g_joinStub = false;

    // SEV-1 2026-10-19: the null-target group join (channels/AnchorGuard.cpp). Only at the self site, only
    // with the widened stub, only when ch.22's own answer did not refuse.
    bool AnswerJoinCheck(RE::Actor* a_this, std::uintptr_t a_target, std::uintptr_t a_group) {
        try {
            return apmf::anchorguard::AnswerJoin(a_this, reinterpret_cast<RE::Actor*>(a_target),
                                                 reinterpret_cast<RE::CombatGroup*>(a_group), g_siteRet);
        } catch (...) {
            return false;   // nothing may unwind into StartCombat
        }
    }

    struct IsDeadHook {
        // `a_r8` / `a_r9`, handed in by the stub. At the TARGET-check site a_r8 = StartCombat's rdi (its
        // `this`). At the SELF-check site, with the widened stub only, a_r8 = rsi (the target) and a_r9 = rbp
        // (the group). Garbage everywhere else; never read there.
        static bool thunk(RE::Actor* a_this, bool a_notEssential, std::uintptr_t a_r8, std::uintptr_t a_r9) {
            const bool dead = func(a_this, a_notEssential);   // the engine's own answer, always
            const auto ret  = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            if (ret == g_siteRet) {
                if (AnswerSelfCheck(a_this, dead)) return true;
                return g_joinStub && !dead && a_this ? AnswerJoinCheck(a_this, a_r8, a_r9) : dead;
            }
            if (ret == g_targetRet && g_targetRet != 0)
                return AnswerTargetCheck(a_this, reinterpret_cast<RE::Actor*>(a_r8), dead);
            return dead;
        }
        using Orig_t = bool(RE::Actor*, bool);   // the engine's IsDead (a function type, as decltype(thunk) was)
        static inline REL::Relocation<Orig_t> func;
    };

    bool SlotStillOurs() {
        if (!g_vtChar || !g_slotFn) return false;
        const auto cur = *reinterpret_cast<const std::uintptr_t*>(g_vtChar + sizeof(void*) * kIsDeadSlot);
        return cur == g_slotFn;
    }

    // The 17-byte stub: mov r8, rdi; jmp qword ptr [rip+0]; <&IsDeadHook::thunk>. With `a_join`
    // (SEV-1 2026-10-19) the 53-byte WIDENED stub instead (see the header block):
    //   48 B8 <imm64 g_siteRet>   mov rax, imm64
    //   48 39 04 24               cmp [rsp], rax
    //   75 14                     jne generic (+20)
    //   49 89 F0                  mov r8, rsi
    //   49 89 E9                  mov r9, rbp
    //   FF 25 00 00 00 00 <8>     jmp [rip+0] <thunk>
    // generic:
    //   49 89 F8                  mov r8, rdi
    //   FF 25 00 00 00 00 <8>     jmp [rip+0] <thunk>
    // Own trampoline, never freed (the engine calls IsDead until the process exits). 0 only on a layout
    // slip (the byte count below). SKSE::Trampoline::allocate never returns null: a request past the pool's
    // free size is report_and_fail (fork src/SKSE/Trampoline.cpp do_allocate), so the pool is sized for BOTH
    // stubs (53 + 17 = 70 <= 128) and the 17-byte fallback in Install can never exhaust it (review F2 of ace1d8b).
    constexpr std::size_t kStubPool = 128;
    std::uintptr_t BuildRdiStub(bool a_join) {
        static auto* s_tr = [] {   // once: a second call (the 17-byte fallback) allocates from the same pool
            auto* tr  = new SKSE::Trampoline("APMF.ReentryDeny.RdiStub");
            void* mem = nullptr;
            if (const auto* intfc = SKSE::GetTrampolineInterface()) mem = intfc->AllocateFromBranchPool(kStubPool);
            if (mem) tr->set_trampoline(mem, kStubPool);
            else tr->create(kStubPool);
            return tr;
        }();
        const auto          target = SKSE::stl::unrestricted_cast<std::uintptr_t>(&IsDeadHook::thunk);
        const std::uint8_t  jmp[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
        const std::uint8_t  movR8Rdi[3] = { 0x49, 0x89, 0xF8 };
        const std::size_t   len    = a_join ? 53 : 17;
        auto*               code   = static_cast<std::uint8_t*>(s_tr->allocate(len));   // never null (see above)
        std::size_t o = 0;
        auto put = [&](const void* p, std::size_t n) {
            std::memcpy(code + o, p, n);
            o += n;
        };
        if (a_join) {
            const std::uint8_t movRax[2] = { 0x48, 0xB8 };
            const std::uint8_t cmpJne[6] = { 0x48, 0x39, 0x04, 0x24, 0x75, 0x14 };
            const std::uint8_t selfRegs[6] = { 0x49, 0x89, 0xF0, 0x49, 0x89, 0xE9 };
            put(movRax, sizeof(movRax));
            put(&g_siteRet, sizeof(g_siteRet));
            put(cmpJne, sizeof(cmpJne));
            put(selfRegs, sizeof(selfRegs));
            put(jmp, sizeof(jmp));
            put(&target, sizeof(target));
        }
        put(movR8Rdi, sizeof(movR8Rdi));
        put(jmp, sizeof(jmp));
        put(&target, sizeof(target));
        if (o != len) return 0;   // a layout slip: refuse rather than install a wrong stub
        REX::W32::FlushInstructionCache(REX::W32::GetCurrentProcess(), code, len);
        return reinterpret_cast<std::uintptr_t>(code);
    }

    std::uint64_t WindowMs(const APMF_API::APMF_Param& param, bool* clamped = nullptr) {
        float w = param.fval;   // ControlMap refused NaN / negative synchronously
        if (!(w > 0.0f)) w = kDefaultWindowSec;
        if (w > kMaxWindowSec) {
            w = kMaxWindowSec;
            if (clamped) *clamped = true;
        }
        return static_cast<std::uint64_t>(std::llround(static_cast<double>(w) * 1000.0));
    }

    // End the WINNING claim this entry was written for (deadline `deadline`): record the reason,
    // EnqueueRelease its handle, log it. MAIN SEAT, OUTSIDE Drain only (Poll / Settle).
    void EndClaim(RE::FormID id, std::uint64_t deadline, const char* why) {
        APMF_API::APMF_Param param{};
        float                basis = 0.0f;
        APMF_API::Handle     h     = APMF_API::kInvalidHandle;
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(id, APMF_API::kIntent_CombatReentryDeny, param, basis,
                                                             &h) ||
            h == APMF_API::kInvalidHandle)
            return;
        {
            std::unique_lock lk(g_mx);
            const auto it = g_denies.find(id);
            if (it == g_denies.end() || it->second.deadlineMs != deadline || it->second.ending.load()) return;
            it->second.ending.store(true);
            it->second.endedReason.store(why);
        }
        apmf::ControlMap::Get().EnqueueRelease(h);
        spdlog::info("[ch.22] 0x{} deny ended: {} (claim h={}). Harbinger released the claim; engine combat "
                     "entries pass again. A mod that wants more time sends a new request or Repoints first.",
                     Hex(id), why, h);
    }

    // Review F3. Posted by Apply(); runs from mainthread::Pump right after Drain PUBLISHED the
    // claim, so the winning handle is readable. Moves the provisional deadline to the winning
    // claim's OWN request / last-Repoint time + its window, and ends a claim whose own window is
    // already over.
    void Settle(RE::FormID id, std::uint32_t gen) {
        APMF_API::APMF_Param param{};
        float                basis = 0.0f;
        APMF_API::Handle     h     = APMF_API::kInvalidHandle;
        if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(id, APMF_API::kIntent_CombatReentryDeny, param, basis,
                                                             &h) ||
            h == APMF_API::kInvalidHandle)
            return;   // released or moved before the pump: nothing to settle
        std::uint64_t atMs = 0;
        {
            std::scoped_lock lk(g_reqMx);
            if (const auto it = g_reqs.find(h); it != g_reqs.end()) atMs = it->second.atMs;
        }
        if (atMs == 0) {
            spdlog::warn("[ch.22] 0x{} claim h={} has no recorded request time; its window runs from when it was "
                         "applied.",
                         Hex(id), h);
            return;
        }
        const auto    now      = apmf::clock::MonotonicMs();
        std::uint64_t deadline = 0, provisional = 0;
        {
            std::unique_lock lk(g_mx);
            const auto it = g_denies.find(id);
            if (it == g_denies.end() || it->second.gen != gen || it->second.ending.load()) return;   // moved on
            provisional             = it->second.deadlineMs;
            deadline                = atMs + WindowMs(param);
            it->second.deadlineMs   = deadline;
            it->second.windowMs     = WindowMs(param);
        }
        if (deadline + 50 < provisional) {   // a takeover (or a pump delay): say what is left
            spdlog::info("[ch.22] 0x{} window runs from claim h={}'s own request: {:.1f} s left.", Hex(id), h,
                         now >= deadline ? 0.0 : static_cast<double>(deadline - now) / 1000.0);
        }
        if (now >= deadline) EndClaim(id, deadline, "window elapsed");
    }

    // (Re)write this actor's entry with a PROVISIONAL deadline (now + window) and post Settle,
    // which moves it to the winning claim's own time. Inside Drain (game thread).
    void Apply(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param, const char* what) {
        bool       clamped  = false;
        const auto windowMs = WindowMs(param, &clamped);
        const auto now      = apmf::clock::MonotonicMs();
        const bool inCombat = actor && HasController(actor);

        std::uint32_t gen = 0;
        {
            std::unique_lock lk(g_mx);
            auto [it, fresh] = g_denies.try_emplace(id);
            Deny& d = it->second;
            d.deadlineMs = now + windowMs;
            d.windowMs   = windowMs;
            d.gen = gen  = ++g_nextGen;
            d.ending.store(false, std::memory_order_relaxed);
            d.endedReason.store(nullptr, std::memory_order_relaxed);
            d.hadController.store(inCombat, std::memory_order_relaxed);
            d.clientPassSeen.store(d.clientPass.load(std::memory_order_relaxed), std::memory_order_relaxed);
            d.sourcePassSeen.store(d.sourcePass.load(std::memory_order_relaxed), std::memory_order_relaxed);
            d.seatHitsSeen.store(d.seatHits.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_count.store(g_denies.size(), std::memory_order_relaxed);
        }
        apmf::mainthread::Post([id, gen] { Settle(id, gen); });

        const bool ours = SlotStillOurs();
        spdlog::info("[ch.22] 0x{} combat re-entry deny {}, window {:.1f} s{}: every engine Actor::StartCombat for "
                     "this actor is refused while the window runs (only a ch.21 entry passes). {} Seat slot 0x99: {}.",
                     Hex(id), what, static_cast<double>(windowMs) / 1000.0,
                     clamped ? fmt::format(" (param.fval {:.1f} clamped to the {:.0f} s maximum)",
                                           static_cast<double>(param.fval), static_cast<double>(kMaxWindowSec))
                             : std::string(),
                     inCombat ? "The actor IS in combat now: Harbinger stops nothing (call Actor::StopCombat "
                                "yourself); its fight gains no new targets through StartCombat."
                              : "The actor is out of combat.",
                     ours ? "Harbinger's" : "NOT Harbinger's");
        if (!ours) {
            spdlog::warn("[ch.22] 0x{} Character::IsDead (vtable slot 0x99) no longer points at Harbinger's seat: "
                         "another DLL wrapped it after Harbinger. This claim denies only if that wrapper tail-jumps "
                         "to the previous entry; watch for 'DENY MISSED'.",
                         Hex(id));
        }
    }

    class CombatReentryDenyChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "combat-reentry-deny"; }
        int              ChannelNo() const override { return 22; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_CombatReentryDeny; }

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            Apply(id, actor, param, "ENGAGED");
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            Apply(id, actor, param, "RE-POINTED");
        }

        // Relinquish (INVARIANTS #5a): nothing to restore -- nothing was written. The engine's
        // combat entries pass again from the next call.
        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            struct { std::uint32_t hits, denied, client, expired, source, missed; } c{};
            bool        had = false;
            const char* why = nullptr;
            {
                std::unique_lock lk(g_mx);
                if (const auto it = g_denies.find(id); it != g_denies.end()) {
                    had = true;
                    const Deny& d = it->second;
                    c = { d.seatHits.load(), d.denied.load(), d.clientPass.load(), d.expiredPass.load(),
                          d.sourcePass.load(), d.missed.load() };
                    why = d.endedReason.load();
                    g_denies.erase(it);
                }
                g_count.store(g_denies.size(), std::memory_order_relaxed);
            }
            {   // the actor's last claim is gone: drop every request time recorded for it
                std::scoped_lock lk(g_reqMx);
                std::erase_if(g_reqs, [id](const auto& kv) { return kv.second.actor == id; });
                g_reqCount.store(g_reqs.size(), std::memory_order_relaxed);
            }
            if (!had) {
                spdlog::info("[ch.22] 0x{} combat re-entry deny released (no entry state).", Hex(id));
                return;
            }
            spdlog::info("[ch.22] 0x{} combat re-entry deny released ({}): StartCombat self-checks seen {} -- "
                         "refused {}, passed as a ch.21 entry {}, passed after the window {}, passed by source "
                         "priority or unattributed {}; DENY MISSED {}. Engine combat entries pass again.",
                         Hex(id), why ? fmt::format("ENDED BY HARBINGER: {}", why)
                                      : std::string("by the client, an unload or a load"),
                         c.hits, c.denied, c.client, c.expired, c.source, c.missed);
        }
    };

}

namespace apmf::reentrydeny {

    ClientEntryScope::ClientEntryScope(RE::FormID actor) : prev_(t_passActor) { t_passActor = actor; }
    ClientEntryScope::~ClientEntryScope() { t_passActor = prev_; }

    void NoteRequest(APMF_API::Handle handle, RE::FormID actor) {
        std::scoped_lock lk(g_reqMx);
        g_reqs.insert_or_assign(handle, Req{ actor, apmf::clock::MonotonicMs() });
        g_reqCount.store(g_reqs.size(), std::memory_order_relaxed);
    }

    void NoteRepoint(APMF_API::Handle handle) {
        if (g_reqCount.load(std::memory_order_relaxed) == 0) return;   // no ch.22 claim anywhere
        std::scoped_lock lk(g_reqMx);
        if (const auto it = g_reqs.find(handle); it != g_reqs.end()) it->second.atMs = apmf::clock::MonotonicMs();
    }

    void Install() {
        if (g_installTried.exchange(true)) return;

        const char* why = nullptr;
        if (REL::Module::IsVR()) {
            why = "VR runtime (the seat is verified on 1.6.1170, 1.5.97 and 1.7.104 only)";
        } else if (!apmf::allowance::RuntimeSupported()) {   // F2b: 1.7.104 proven (StartCombat + slot 0x99 identical)
            why = "runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104 (the seat is verified on those three only)";
        } else if (GetPrivateProfileIntA("CombatReentryDeny", "bCombatReentryDeny", 1, kIni) == 0) {
            why = "[CombatReentryDeny] bCombatReentryDeny=0 in Data/SKSE/Plugins/APMF.ini";
        }
        if (why) {
            g_notInstalledReason.store(why, std::memory_order_release);
            spdlog::warn("[ch.22] combat re-entry deny seat NOT installed -- {}. kIntent_CombatReentryDeny claims "
                         "are REFUSED.",
                         why);
            return;
        }

        // Verify BOTH rows before writing anything.
        REL::Relocation<std::uintptr_t> vtChar{ RE::VTABLE_Character[0] };
        const std::uintptr_t startCombat = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(37608, 38561) }.address();
        const std::uintptr_t site =
            startCombat + (apmf::allowance::RuntimeAEColumn() ? kSelfCheckOffsetAE : kSelfCheckOffsetSE);   // F2b: 1.7.104 = +0x8F
        bool ok = apmf::allowance::SeatVerified(vtChar.address(), "CombatReentryDeny.Character.IsDead(0x99)");
        ok = apmf::allowance::SeatVerified(site, "ReentryDeny.StartCombat.SelfIsDeadCall") && ok;
        if (!ok) {
            g_notInstalledReason.store("the address self-check refused the Character vtable or StartCombat's "
                                       "self-check call site",
                                       std::memory_order_release);
            spdlog::error("[ch.22] combat re-entry deny seat NOT installed (self-check refused a row, listed "
                          "above). kIntent_CombatReentryDeny claims are REFUSED.");
            return;
        }

        g_siteRet = site + kSelfCheckLen;
        g_vtChar  = vtChar.address();
        // The combat substrate's second return site (S2a): its own verified row and the rdi stub. A refusal
        // here leaves the seat exactly the ch.22 seat (the thunk itself in the slot; the substrate refuses).
        const std::uintptr_t tsite =
            startCombat + (apmf::allowance::RuntimeAEColumn() ? kTargetCheckOffsetAE : kTargetCheckOffsetSE);
        std::uintptr_t stub = 0;
        bool           join = false;
        if (apmf::allowance::SeatVerified(tsite, "Substrate.StartCombat.TargetIsDeadCall")) {
            // SEV-1 2026-10-19: the widened stub only when StartCombat's register copies verify (row
            // AnchorGuard.StartCombat.TargetGroupRegs; 1.7.104 refused by name inside InstallSeat).
            join = apmf::anchorguard::InstallSeat(startCombat);
            stub = BuildRdiStub(join);
            if (!stub && join) {
                spdlog::error("[ch.22] the widened rdi/rsi/rbp stub could not be built: the anchor join guard is OFF.");
                join = false;
                stub = BuildRdiStub(false);
            }
        }
        g_joinStub = stub && join;   // before the slot write: the thunk reads it
        g_slotFn = stub ? stub : SKSE::stl::unrestricted_cast<std::uintptr_t>(&IsDeadHook::thunk);
        IsDeadHook::func = vtChar.write_vfunc(kIsDeadSlot, g_slotFn);
        if (stub) g_targetRet = tsite + kTargetCheckLen;   // only after the slot holds the stub
        if (g_joinStub) apmf::anchorguard::SeatArmed();
        apmf::sources::NoteVfunc(apmf::sources::Facet::kCombat, "ReentryDeny.Character.IsDead", vtChar.address(),
                                 kIsDeadSlot, g_slotFn, IsDeadHook::func.address());
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[ch.22] combat re-entry deny seat installed: Character::IsDead (slot 0x{:X}, chaining), "
                     "answering only at Actor::StartCombat's self-check (return RVA 0x{:X}){}.",
                     kIsDeadSlot, g_siteRet - REL::Module::get().base(),
                     g_targetRet ? fmt::format(" and, for the combat substrate's anchor, at its TARGET check (return RVA "
                                               "0x{:X}, through the rdi stub); anchor join guard at the self check: {}",
                                               g_targetRet - REL::Module::get().base(),
                                               apmf::anchorguard::SeatState())
                                 : std::string(" (the substrate's anchor refusal is NOT installed: its row or the stub "
                                               "failed)"));
    }

    bool Installed() { return g_installed.load(std::memory_order_relaxed); }

    bool TargetSiteInstalled() { return g_installed.load(std::memory_order_acquire) && g_targetRet != 0; }

    const char* NotInstalledReason() {
        const char* r = g_notInstalledReason.load(std::memory_order_acquire);
        return r ? r : "unknown";
    }

    void Poll() {
        if (g_count.load(std::memory_order_relaxed) == 0) return;
        static std::uint64_t s_lastMs = 0;
        const std::uint64_t  now      = apmf::clock::MonotonicMs();
        if (now - s_lastMs < 250) return;
        s_lastMs = now;

        struct Snap { RE::FormID id; std::uint64_t deadline; };
        std::vector<Snap> snaps;
        {
            std::shared_lock lk(g_mx);
            snaps.reserve(g_denies.size());
            for (const auto& [id, d] : g_denies)
                if (!d.ending.load(std::memory_order_relaxed)) snaps.push_back({ id, d.deadlineMs });
        }

        for (const auto& sn : snaps) {
            auto*       owner = RE::TESForm::LookupByID<RE::Actor>(sn.id);
            const char* why   = nullptr;
            if (now >= sn.deadline)       why = "window elapsed";
            else if (owner && owner->IsDead()) why = "owner dead";

            if (!why && owner) {
                // DENY-MISS detector (principle 7): out of combat at the last look, in combat now,
                // and no ch.21 entry passed the seat in between.
                const bool hasCc = HasController(owner);
                std::shared_lock lk(g_mx);
                const auto it = g_denies.find(sn.id);
                if (it == g_denies.end() || it->second.deadlineMs != sn.deadline) continue;   // moved on
                const Deny& d        = it->second;
                const bool  prev     = d.hadController.exchange(hasCc, std::memory_order_relaxed);
                const auto  cp       = d.clientPass.load(std::memory_order_relaxed);
                const auto  cpSeen   = d.clientPassSeen.exchange(cp, std::memory_order_relaxed);
                const auto  sp       = d.sourcePass.load(std::memory_order_relaxed);
                const auto  spSeen   = d.sourcePassSeen.exchange(sp, std::memory_order_relaxed);
                const auto  hits     = d.seatHits.load(std::memory_order_relaxed);
                const auto  hitsSeen = d.seatHitsSeen.exchange(hits, std::memory_order_relaxed);
                // Harbinger 1b: an entry a source that is not outranked made (it passed the seat by
                // priority, or unattributed) is not a miss either.
                if (!prev && hasCc && cp == cpSeen && sp == spSeen) {
                    const auto n = d.missed.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (RateOk(d.lastMissLogMs)) {
                        spdlog::warn("[ch.22] 0x{} DENY MISSED: the actor ENTERED COMBAT while its re-entry deny held, "
                                     "and no ch.21 entry (nor a source the claim does not outrank) passed the seat. StartCombat self-checks seen for it since "
                                     "the last look: {}{}. ({} so far)",
                                     Hex(sn.id), hits - hitsSeen,
                                     hits == hitsSeen ? " -- the seat never saw this entry (another DLL wrapping "
                                                        "Character::IsDead without a tail jump, or an engine path "
                                                        "this channel does not know)"
                                                      : "",
                                     n);
                    }
                }
                continue;
            }
            if (!why) continue;

            EndClaim(sn.id, sn.deadline, why);
        }
    }

    void ResetAll(const char* why) {
        std::size_t n = 0;
        {
            std::unique_lock lk(g_mx);
            n = g_denies.size();
            g_denies.clear();
            g_count.store(0, std::memory_order_relaxed);
        }
        {
            std::scoped_lock lk(g_reqMx);
            g_reqs.clear();
            g_reqCount.store(0, std::memory_order_relaxed);
        }
        if (n != 0) spdlog::info("[ch.22] {} -- dropped {} re-entry deny entr{}.", why, n, n == 1 ? "y" : "ies");
    }

}

APMF_REGISTER_CHANNEL(CombatReentryDenyChannel);
