#include "PCH.h"
#include "channels/CombatReentryDeny.h"
#include "core/Allowance.h"   // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/Registry.h"

#include <cmath>
#include <intrin.h>   // _ReturnAddress (core/EquipSink.cpp / core/EquipGate.cpp precedent)
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
// 120 s). While that is the WINNING kIntent_CombatReentryDeny claim on the actor, the window
// is running and the actor is OUT OF COMBAT (no combat controller), the engine's own
// COMBAT ENTRY for that actor is REFUSED at its source. Nothing else changes: no StopCombat,
// no engine state written, no other facet touched.
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
//     * its window has not elapsed;
//     * the actor has NO combat controller (the pointer only; nothing behind it is read):
//       the facet is ENTRY. An actor already fighting is not stopped and the engine may still
//       add targets to its group (StartCombat's in-combat path) -- the client decides whether
//       the fight ends (Actor::StopCombat, its call);
//     * the call is not ch.21's own entry for this actor (ClientEntryScope, below).
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
// combat controller while its window runs without a ch.21 entry passing the seat is logged
// loudly (principle 7). The known way to miss: another DLL wraps Character slot 0x99 AFTER
// Harbinger with a thunk that CALLS (not tail-jumps to) the previous entry; StartCombat's return
// address is then theirs, never ours. Apply() warns when the slot no longer holds this thunk.
//
// THE CLAIM ENDS, AND HARBINGER RELEASES IT ITSELF (logged "deny ended: <reason>", repeated on
// the Release line), when the window elapses or the owner dies -- Poll(). A client's Release,
// an outranking claim, a save load / new game (claims are never saved; ResetAll) and the
// actor unloading also end it. Repoint(handle, &param) restarts the window from that moment.
// Nothing is undone on any end: nothing was written.
//
// DENY-COMPLETENESS (principle 2, 2026-09-25 scope: the client owns the consequences). The
// facet is "this actor ENTERS combat". Competing sources: every engine / script / mod
// StartCombat on this actor -- DENIED at the one choke point (above); the save-load rebuild --
// out of scope (no claim crosses a load). NOT denied, by design: other actors fighting THIS
// actor (their StartCombat has them as `this`; hits, spells and pursuit are their facets); an
// actor that is already in combat. Harbinger switches no facet on.
//
// THREADING. StartCombat runs on whatever thread asks (the main thread, BSJobs combat /
// detection jobs, the script VM). The seat reads the ControlMap's published snapshot
// (lock-free) and this file's map under a shared_lock (written on the game thread under a
// unique_lock), plus the actor's own controller POINTER. It dereferences no controller and
// takes no engine lock. The engine's own spinlock is not yet held at the seat.
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

    std::atomic<bool>        g_installed{ false };
    std::atomic<bool>        g_installTried{ false };
    std::atomic<const char*> g_notInstalledReason{ "before kDataLoaded (the seat installs there)" };

    // Written once at Install, before the vtable write that makes the thunk reachable.
    std::uintptr_t g_siteRet = 0;   // StartCombat's self-check return address
    std::uintptr_t g_vtChar  = 0;   // the Character vtable the thunk sits on

    // Principle 5: every StartCombat self-check the seat saw, claim or no claim.
    std::atomic<std::uint64_t> g_siteSeen{ 0 };

    thread_local RE::FormID t_passActor = 0;   // ClientEntryScope: ch.21's own entry, this thread

    // One entry per actor with an engaged ch.22. Written ONLY on the game thread under a
    // unique_lock; read by the seat on any thread under a shared_lock. Everything the seat or
    // Poll mutates is atomic, so a shared_lock is enough for them.
    struct Deny {
        std::uint64_t deadlineMs = 0;   // monotonic ms; the window ends here
        float         windowSec  = 0.0f;
        // seat
        mutable std::atomic<std::uint32_t> seatHits{ 0 };      // StartCombat self-checks for this actor
        mutable std::atomic<std::uint32_t> denied{ 0 };        // entries refused
        mutable std::atomic<std::uint32_t> inCombatPass{ 0 };  // passed: already in combat
        mutable std::atomic<std::uint32_t> clientPass{ 0 };    // passed: ch.21's own entry
        mutable std::atomic<std::uint32_t> expiredPass{ 0 };   // passed: window over, Poll not yet run
        mutable std::atomic<bool>          firstDenyLogged{ false };
        // Poll (game thread)
        mutable std::atomic<bool>          hadController{ false };
        mutable std::atomic<std::uint32_t> clientPassSeen{ 0 };
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

    bool HasController(RE::Actor* a) {
        return a->GetActorRuntimeData().combatController != nullptr;   // the POINTER only
    }

    bool RateOk(std::atomic<std::uint64_t>& last) {
        const auto now  = apmf::clock::MonotonicMs();
        auto       prev = last.load(std::memory_order_relaxed);
        return now - prev >= kLogEveryMs && last.compare_exchange_strong(prev, now, std::memory_order_relaxed);
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
        if (g_count.load(std::memory_order_relaxed) == 0) return engineDead;

        const RE::FormID     self = a_this->GetFormID();
        APMF_API::APMF_Param claim{};
        if (!apmf::ControlMap::Get().TryGetOwningClaim(self, APMF_API::kIntent_CombatReentryDeny, claim))
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
        if (HasController(a_this)) {                                       // fighting: not an entry
            d.inCombatPass.fetch_add(1, std::memory_order_relaxed);
            return engineDead;
        }

        d.denied.fetch_add(1, std::memory_order_relaxed);   // DENY: StartCombat refuses, nothing done
        if (!d.firstDenyLogged.exchange(true, std::memory_order_relaxed)) {
            spdlog::info("[ch.22] 0x{} FIRST DENY: the engine asked to put the actor into combat "
                         "(Actor::StartCombat); refused at its own self-check, before it did anything. "
                         "{:.1f} s of the window left.",
                         Hex(self), static_cast<double>(d.deadlineMs - now) / 1000.0);
        }
        return true;
    }

    struct IsDeadHook {
        static bool thunk(RE::Actor* a_this, bool a_notEssential) {
            const bool dead = func(a_this, a_notEssential);   // the engine's own answer, always
            if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) != g_siteRet) return dead;
            return AnswerSelfCheck(a_this, dead);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    bool SlotStillOurs() {
        if (!g_vtChar) return false;
        const auto cur = *reinterpret_cast<const std::uintptr_t*>(g_vtChar + sizeof(void*) * kIsDeadSlot);
        return cur == reinterpret_cast<std::uintptr_t>(&IsDeadHook::thunk);
    }

    // (Re)write this actor's entry: the window restarts now. Inside Drain (game thread).
    void Apply(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param, const char* what) {
        float w       = param.fval;   // ControlMap refused NaN / negative synchronously
        bool  clamped = false;
        if (!(w > 0.0f)) w = kDefaultWindowSec;
        if (w > kMaxWindowSec) {
            w       = kMaxWindowSec;
            clamped = true;
        }
        const auto now      = apmf::clock::MonotonicMs();
        const auto deadline = now + static_cast<std::uint64_t>(std::llround(static_cast<double>(w) * 1000.0));
        const bool inCombat = actor && HasController(actor);

        {
            std::unique_lock lk(g_mx);
            auto [it, fresh] = g_denies.try_emplace(id);
            Deny& d = it->second;
            d.deadlineMs = deadline;
            d.windowSec  = w;
            d.ending.store(false, std::memory_order_relaxed);
            d.endedReason.store(nullptr, std::memory_order_relaxed);
            d.hadController.store(inCombat, std::memory_order_relaxed);
            d.clientPassSeen.store(d.clientPass.load(std::memory_order_relaxed), std::memory_order_relaxed);
            d.seatHitsSeen.store(d.seatHits.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_count.store(g_denies.size(), std::memory_order_relaxed);
        }

        spdlog::info("[ch.22] 0x{} combat re-entry deny {} for {:.1f} s{}: the engine's combat ENTRY for this "
                     "actor is refused at Actor::StartCombat while the window runs and the actor is out of "
                     "combat. {}",
                     Hex(id), what, static_cast<double>(w),
                     clamped ? fmt::format(" (param.fval {:.1f} clamped to the {:.0f} s maximum)",
                                           static_cast<double>(param.fval), static_cast<double>(kMaxWindowSec))
                             : std::string(),
                     inCombat ? "The actor IS in combat now: Harbinger stops nothing; entries are refused once "
                                "the fight ends (call Actor::StopCombat yourself)."
                              : "The actor is out of combat.");
        if (!SlotStillOurs()) {
            static std::atomic<bool> s_warned{ false };
            if (!s_warned.exchange(true)) {
                spdlog::warn("[ch.22] Character::IsDead (vtable slot 0x99) no longer points at Harbinger's seat: "
                             "another DLL wrapped it after Harbinger. The deny works only if that wrapper "
                             "tail-jumps to the previous entry; watch for 'DENY MISSED'. (Logged once.)");
            }
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
            struct { std::uint32_t hits, denied, inCombat, client, expired, missed; } c{};
            bool        had = false;
            const char* why = nullptr;
            {
                std::unique_lock lk(g_mx);
                if (const auto it = g_denies.find(id); it != g_denies.end()) {
                    had = true;
                    const Deny& d = it->second;
                    c = { d.seatHits.load(), d.denied.load(), d.inCombatPass.load(), d.clientPass.load(),
                          d.expiredPass.load(), d.missed.load() };
                    why = d.endedReason.load();
                    g_denies.erase(it);
                }
                g_count.store(g_denies.size(), std::memory_order_relaxed);
            }
            if (!had) {
                spdlog::info("[ch.22] 0x{} combat re-entry deny released (no entry state).", Hex(id));
                return;
            }
            spdlog::info("[ch.22] 0x{} combat re-entry deny released ({}): StartCombat self-checks seen {} -- "
                         "entries refused {}, passed because already in combat {}, passed as a ch.21 entry {}, "
                         "passed after the window {}; DENY MISSED {}. Engine combat entries pass again.",
                         Hex(id), why ? fmt::format("ENDED BY HARBINGER: {}", why)
                                      : std::string("by the client, an unload or a load"),
                         c.hits, c.denied, c.inCombat, c.client, c.expired, c.missed);
        }
    };

}

namespace apmf::reentrydeny {

    ClientEntryScope::ClientEntryScope(RE::FormID actor) : prev_(t_passActor) { t_passActor = actor; }
    ClientEntryScope::~ClientEntryScope() { t_passActor = prev_; }

    void Install() {
        if (g_installTried.exchange(true)) return;

        const char* why = nullptr;
        if (REL::Module::IsVR()) {
            why = "VR runtime (the seat is verified on 1.6.1170 and 1.5.97 only)";
        } else if (!REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) &&
                   !REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97)) {
            why = "runtime is not exactly 1.6.1170 or 1.5.97 (the seat is verified on those two only)";
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
            startCombat + (REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) ? kSelfCheckOffsetAE : kSelfCheckOffsetSE);
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
        IsDeadHook::func = vtChar.write_vfunc(kIsDeadSlot, IsDeadHook::thunk);
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[ch.22] combat re-entry deny seat installed: Character::IsDead (slot 0x{:X}, chaining), "
                     "answering only at Actor::StartCombat's self-check (return RVA 0x{:X}).",
                     kIsDeadSlot, g_siteRet - REL::Module::get().base());
    }

    bool Installed() { return g_installed.load(std::memory_order_relaxed); }

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
                const auto  hits     = d.seatHits.load(std::memory_order_relaxed);
                const auto  hitsSeen = d.seatHitsSeen.exchange(hits, std::memory_order_relaxed);
                if (!prev && hasCc && cp == cpSeen) {
                    const auto n = d.missed.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (RateOk(d.lastMissLogMs)) {
                        spdlog::warn("[ch.22] 0x{} DENY MISSED: the actor ENTERED COMBAT while its re-entry deny held, "
                                     "and no ch.21 entry passed the seat. StartCombat self-checks seen for it since "
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

            // End the WINNING claim -- the one this entry was written for.
            APMF_API::APMF_Param param{};
            float                basis = 0.0f;
            APMF_API::Handle     h     = APMF_API::kInvalidHandle;
            if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(sn.id, APMF_API::kIntent_CombatReentryDeny, param,
                                                                 basis, &h) ||
                h == APMF_API::kInvalidHandle)
                continue;
            {
                std::unique_lock lk(g_mx);
                const auto it = g_denies.find(sn.id);
                if (it == g_denies.end() || it->second.deadlineMs != sn.deadline || it->second.ending.load()) continue;
                it->second.ending.store(true);
                it->second.endedReason.store(why);
            }
            apmf::ControlMap::Get().EnqueueRelease(h);
            spdlog::info("[ch.22] 0x{} deny ended: {} (claim h={}). Harbinger released the claim; engine combat "
                         "entries pass again. A mod that wants more time sends a new request or Repoints first.",
                         Hex(sn.id), why, h);
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
        if (n != 0) spdlog::info("[ch.22] {} -- dropped {} re-entry deny entr{}.", why, n, n == 1 ? "y" : "ies");
    }

}

APMF_REGISTER_CHANNEL(CombatReentryDenyChannel);
