#pragma once
#include "APMF_API.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// ============================================================================
// core/Sources -- HARBINGER CONTROLLER, Phase 1a: the generic SOURCE REGISTRY,
// RECOGNITION, PRIORITIES (APMF_Sources.ini), the DEGRADE RULE and the SEAT CENSUS.
// Plan: _research/harbinger-controller-plan-2026-10-11.md ("Generic machinery") with
// marth's answers Q1-Q15 of the same day.
//
// THE MODEL. Harbinger owns each facet's hook point. Anything else that writes the
// facet is an INPUT from a SOURCE. A source is {name, kind}: a client DLL (identified
// from the calling module of ClientAPI, `_ReturnAddress()`), a hook / interposer DLL
// (found by the census on one of Harbinger's own seats), a plugin (.esp/.esm/.esl that
// owns a package or scene), the engine, "direct calls" (Papyrus / console, Q11) or
// "Unattributed". Each (source, facet) has a PRIORITY 0..100 (0 = off, Q6): defaults
// 20 for an unregistered outside source, 50 for a client, 75 for direct calls (Q11);
// the player's override lives in Data/SKSE/Plugins/APMF_Sources.ini, keyed by NAME
// (Q8), per facet and global (Q9).
//
// THE DEGRADE RULE (principle 7, Q4). Harbinger enforces ONLY against a positively
// identified source that has been outranked. An outside write it cannot attribute is
// ALLOWED, with a loud rate-limited `[sources] UNATTRIBUTED <facet>` line, a hit on the
// "Unattributed" source and an ALERT row on the SKSE Menu page. It is never denied.
//
// THE SEAT CENSUS. Every seat Harbinger installs is registered here with the value it
// displaced (its module = a hook installed BEFORE Harbinger, i.e. inside the chain). The
// census re-reads every seat at kPostLoadGame / kNewGame and every 60 s on the main-
// thread pump (pointer reads only). A seat whose entry no longer holds Harbinger's own
// pointer was hooked over by a LATE installer: its module becomes a hook source on that
// seat's facet, logged once, loudly. A seat registered with a re-hook callback (Q13:
// "re-hook on top") gets Harbinger back on top; the others report "lost outermost".
//
// THREADING. Registry: a fixed append-only array; a name and kind are written before
// the count is published (release), so readers on any thread (combat jobs, the menu
// render thread) scan it lock-free. Counters and priorities are atomics. Registration
// takes one mutex (rare). The census and the priority writes run on the MAIN thread
// (Arbiter::OncePerFrame -> Pump); the menu only ENQUEUES a slider change.
// ============================================================================

namespace apmf::sources {

    enum class Facet : std::uint8_t {
        kTarget = 0,     // ch.6 / ch.20: which actor an NPC fights
        kDetection,      // the detection-value producer (SmartNPC H2 shape)
        kPackage,        // ch.9 / ch.19: the running AI package (alias, quest, scene, created)
        kEquip,          // ch.15 / ch.17 / the hand block: engine equips
        kCombatAction,   // ch.7 / ch.23: the combat behaviour tree leaves (census only in 1a)
        kCast,           // ch.8 / 8a / 8b: cast choice and execution (census only in 1a)
        kCombat,         // ch.21 / 22 / 24: entry, re-entry, approach (census only in 1a)
        kCore,           // the 0xAD Update seat and other spine seats (census only)
        kCount
    };
    inline constexpr int kFacetCount = static_cast<int>(Facet::kCount);

    enum class Kind : std::uint8_t {
        kClient = 0,    // a DLL that called the APMF client API
        kHook,          // a DLL found on one of Harbinger's vtable seats
        kInterposer,    // a DLL found on one of Harbinger's call sites
        kPlugin,        // a data plugin owning a package / scene, or an SKSE DLL calling the engine directly
        kScript,        // skse64 (Papyrus natives)
        kEngine,        // SkyrimSE.exe's own path
        kDirect,        // "direct calls": Papyrus / console reached through the engine (Q11)
        kUnknown        // the Unattributed source
    };

    const char* FacetName(Facet f);   // the INI section and menu label ("Target", ...)
    // The facet of an existing seat from its allowance::InstallOnVtables tag ("ch.7",
    // "t2a", "ch.8b-seat06", ...): census rows only.
    Facet FacetForTag(std::string_view tag);
    const char* KindName(Kind k);

    inline constexpr int kPrioMin             = 0;    // 0 = off: never wins against anything
    inline constexpr int kPrioMax             = 100;
    inline constexpr int kDefaultUnregistered = 20;   // Q6
    inline constexpr int kDefaultClient       = 50;   // Q6
    inline constexpr int kDefaultDirect       = 75;   // Q11

    using SourceId = int;
    inline constexpr SourceId kNoSource = -1;

    // Built-in sources, registered by Init() at these fixed ids.
    inline constexpr SourceId kEngineSource       = 0;   // "Engine" (SkyrimSE.exe)
    inline constexpr SourceId kDirectSource       = 1;   // "DirectCalls" (Papyrus / console)
    inline constexpr SourceId kUnattributedSource = 2;   // "Unattributed"

    // kDataLoaded, MAIN thread, before any Install(). Reads APMF_Sources.ini (defaults +
    // every [<Facet>] key) and registers the built-ins. Idempotent.
    void Init();

    // Find by exact name (case-insensitive ASCII). Any thread, lock-free.
    SourceId Find(std::string_view name);
    // Find or add. Any thread (one mutex on the add). Returns kNoSource when the fixed
    // table is full (logged once).
    SourceId Register(std::string_view name, Kind kind);
    // The source owning a code address: the module's base name ("SmartNPCTargetSelector.dll"),
    // SkyrimSE.exe -> kEngineSource, skse64*.dll -> kDirectSource. Returns kNoSource for an
    // address inside APMF itself, and for one in no module (a trampoline page) after
    // following up to three jmp stubs. Any thread (GetModuleHandleEx, no lock but the add).
    SourceId ForAddress(std::uintptr_t addr, Kind kind);
    // The base name of the module owning `addr` (after following jmp stubs), "" if none,
    // "APMF.dll" for ourselves. Writes into `out` (>= 64 bytes). Any thread.
    const char* ModuleNameOf(std::uintptr_t addr, char* out, std::size_t size);
    // Follow `jmp rel32` / `jmp [rip+0]` / `mov rax,imm64; jmp rax` stubs, up to 3 hops.
    std::uintptr_t FollowStubs(std::uintptr_t addr);

    std::string_view Name(SourceId id);   // "" for an invalid id
    bool             IsClient(SourceId id);   // it has filed a claim (it may also be a hook source)
    // Is `addr` inside APMF's own image? Any thread (cached module ranges).
    bool IsSelf(std::uintptr_t addr);
    // The image range of the module owning `addr` (jmp stubs followed). Any thread.
    bool ModuleRange(std::uintptr_t addr, std::uintptr_t& lo, std::uintptr_t& hi);
    Kind             KindOf(SourceId id);

    // Priority of a source on a facet (0..100). Any thread, lock-free.
    int Priority(SourceId id, Facet f);

    // Counters. Any thread, lock-free. NoteHit also marks the source as involved in the
    // facet (it appears on the menu's facet table) and stamps "last seen".
    void NoteHit(SourceId id, Facet f);
    void NoteOverruled(SourceId id, Facet f);   // Harbinger enforced against it
    void NoteStood(SourceId id, Facet f);       // it outranked the claim (or tied): its write stood
    void NoteInvolved(SourceId id, Facet f);    // seen on the facet, no hit (census)

    // ---- clients (Q7: automatic from the calling DLL) --------------------------------
    // ClientAPI calls this on every Request / RequestEx / RequestCast that returned a
    // handle, with its own `_ReturnAddress()` (an address inside the client DLL). Any thread.
    void NoteClientRequest(APMF_API::Handle h, const void* returnAddress, APMF_API::Intent intent);
    // The facet a client intent claims (for the menu's per-facet tables).
    Facet FacetForIntent(APMF_API::Intent intent);
    void ForgetHandle(APMF_API::Handle h);   // ClientAPI Release. Any thread.
    // The client source that filed `h`, or kNoSource (an internal claim, e.g. ch.19's own
    // ch.9 offer, or a handle older than this session). Any thread (shared lock).
    SourceId ClientForHandle(APMF_API::Handle h);
    // The priority Harbinger enforces a claim with on `f`: its client's priority, or the
    // client default (50) for an internal / unknown handle. Any thread.
    int ClaimPriority(APMF_API::Handle h, Facet f, SourceId* outClient = nullptr);

    // ---- the degrade rule (Q4) ------------------------------------------------------
    // An outside write on `f` Harbinger could not attribute. ALLOWED by the caller; this
    // logs (rate-limited per facet), counts it on the Unattributed source and adds an
    // alert row for the menu. Any thread.
    void Unattributed(Facet f, RE::FormID actor, const char* detail);

    struct Alert {
        std::uint64_t ms     = 0;
        Facet         facet  = Facet::kTarget;
        RE::FormID    actor  = 0;
        char          detail[128]{};
    };
    std::vector<Alert> RecentAlerts();      // newest first, at most 32. Any thread (brief lock).
    std::uint64_t      AlertCount();        // total since launch. Any thread.

    // ---- the seat census -----------------------------------------------------------
    // Register a VTABLE seat right after write_vfunc: `entry` = the vtable entry's address
    // (vtable + idx*8), `ours` = the thunk written there, `prev` = what write_vfunc
    // returned. The census reads `entry` again later. `rehook` (optional, Q13): called on
    // the MAIN thread when the census finds `entry` no longer == ours; it installs a new
    // Harbinger layer on top and returns the NEW value it wrote (0 = could not).
    // MAIN thread (kDataLoaded installs).
    void RegisterVtableSeat(Facet f, std::string seat, std::uintptr_t entry, std::uintptr_t ours,
                            std::uintptr_t prev, std::function<std::uintptr_t(std::uintptr_t)> rehook = {});
    // The same, from a vtable address and a slot index.
    inline void NoteVfunc(Facet f, std::string seat, std::uintptr_t vtable, std::size_t idx, std::uintptr_t ours,
                          std::uintptr_t prev) {
        RegisterVtableSeat(f, std::move(seat), vtable + sizeof(void*) * idx, ours, prev);
    }
    // Register a CALL-SITE seat right after write_call<5>: `site` = the E8's address,
    // `prevTarget` = what write_call returned. Ours is read back from the E8 now.
    void RegisterCallSeat(Facet f, std::string seat, std::uintptr_t site, std::uintptr_t prevTarget);
    // The module recorded at install as the displaced value of `seat` (a hook INSIDE
    // Harbinger's seat), or kNoSource for the engine / none. Any thread.
    SourceId PrevSourceOf(std::string_view seat);
    // Re-read every seat. MAIN thread. kPostLoadGame / kNewGame and the 60 s pump.
    void Census(const char* why);

    struct SeatView {
        std::string   seat;
        Facet         facet = Facet::kCore;
        bool          callSite = false;
        SourceId      prev = kNoSource;    // the module inside (installed before Harbinger)
        SourceId      late = kNoSource;    // the module on top (installed after Harbinger)
        bool          lostOutermost = false;
        int           rehooks = 0;
    };
    // Published snapshot (seats that involve a foreign module, plus totals). Any thread.
    std::shared_ptr<const std::vector<SeatView>> SeatSnapshot();
    std::uint32_t SeatCount();

    // ---- the pump + the menu's write path -------------------------------------------
    // MAIN thread, once per frame (Arbiter::OncePerFrame): applies queued slider changes
    // (atomic store + INI write), runs the 60 s census, purges released handles.
    void Pump();
    // kPostLoadGame: advance the load sequence (idle greying, Q14) and run the census.
    void OnPostLoad(const char* why);
    // The render thread's slider. Clamped 0..100; applied and saved by the next Pump().
    void EnqueueSetPriority(SourceId id, Facet f, int prio);

    // Menu reads. Any thread, lock-free.
    std::uint32_t Count();
    bool          Involved(SourceId id, Facet f);
    std::uint32_t Hits(SourceId id, Facet f);
    std::uint32_t Overruled(SourceId id, Facet f);
    std::uint32_t Stood(SourceId id, Facet f);
    std::uint64_t LastSeenMs(SourceId id, Facet f);   // 0 = never
    // Q14: a source not seen during the current or the two previous game loads.
    bool          Idle(SourceId id);

}
