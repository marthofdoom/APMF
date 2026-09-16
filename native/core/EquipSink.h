#pragma once

#include <cstddef>
#include <cstdint>

// ============================================================================
// core/EquipSink -- THE ENGINE-EQUIP SINK SEAT (ch.17 EquipAuthority's deny half).
//
// THE FACET. Every engine equip DECISION -- outfit re-apply, the AI's own
// weapon/armor choice, combat re-arm, RemoveItem's re-equip, a Papyrus/console
// EquipItem, another plugin's ActorEquipManager::EquipObject call -- funnels
// through ONE non-virtual worker (AE 38929 / SE 37974), reached from exactly two
// internal call sites inside the engine's own EquipObject bodies (full E8 scan of
// .text on both unpacked binaries, 2026-09-15: no other E8, no E9, no lea, no
// pointer reference). No virtual function exists anywhere on that path
// (`Actor::AddWornItem` is devirtualised), so this facet has NO vtable seat.
// ONE path runs BELOW the seat and is not a decision: the worker's apply function
// (38001, worker +0x289) is also reached from 16073+0xf0 (InventoryChanges family,
// <- 40746 <- 40655), which re-applies PERSISTED ExtraWorn state when an actor's
// 3D loads -- it restores what was worn, it never chooses. 38919 is an orphan
// copy of the worker's tail with no references. (Fable tier-3 on d1aa66b, SEV-4 #5.)
// That is the exact shape Docs/INVARIANTS.md #17a licenses: a bounded call-site
// seat on an INTERNAL choke point, byte-verified at install, refusing the whole
// seat on any mismatch, scoped to actors holding an explicit claim, and
// engine-answer-first in the only form a sink allows (deny == do not call the
// worker; nothing is manufactured). READ #17a's five conditions before touching
// this file -- every one is mandatory, and this seat exists ONLY under them.
//
// WHAT IT DOES. For an actor with a winning kIntent_EquipAuthority claim whose
// client has DECLARED a worn set (APMF_API_v7::SetEquipSet), the thunk lets the
// worker run for a declared item and REFUSES it (returns without calling the
// worker: nothing queued, no re-entry) for an off-set item. The player and an
// unclaimed actor pass through untouched and unlogged (#17a condition 5,
// INVARIANTS #13); a CLAIMED actor with no declaration passes too, but IS logged
// (`verdict=allow`, its path named) -- that is the probe's view of what the engine
// does before a client declares. Equips APMF itself issues
// (channels/EquipAuthority.cpp enforcing a declaration) run inside the TLS
// bracket below; they pass because they are IN THE SET, never because of the
// bracket -- `tls` is a log field that PROVES the equip was APMF's, not a bypass
// (the worker family's 38913/37957 re-entry equips the same in-set object; the
// deferred apply of a queued equip comes back one actor-update later from
// 38906/37950 = `QueuedApply`, tls=0, also in-set). Papyrus/console equips pass unless the claim or the INI says
// kEquipAuth_DenyScript. The PLAYER's own equips on the actor through the
// trade/gift menu (`path=PlayerMenu`) pass unless the claim says
// kEquipAuth_DenyPlayerMenu (ABI v8: player agency; logged `verdict=allow
// (player agency)`). Observe-only (INI, default ON for the first field build,
// or the claim's kEquipAuth_ObserveOnly) logs `would-deny` and refuses nothing.
//
// THE GOVERNED TYPES (ABI v8). EquipObject is also how a potion is drunk, food
// eaten, a scroll read, an ingredient tasted, a book read -- Actor::DrinkPotion
// and the AI's own potion use end at this same worker. Only ARMO / WEAP / AMMO
// / LIGH (torch) are governed; every other form type passes the seat for a
// claimed actor untouched, logged once per (actor, formType) at debug level.
// The equip half (channels/EquipAuthority.cpp) applies the same set: a declared
// item of any other type is skipped, never equipped (equipping it consumes it).
// The v8 per-item HAND (SetEquipSetEx) is an equip-side matter too: the seat's
// in-set test stays a FormID compare, whichever hand the engine aims at.
//
// THE SCOPE (ABI v9, SetEquipScope). v7/v8 held the WHOLE facet: every off-set
// governed equip was refused. v9 holds only the categories the claim OWNS and
// refuses outright the ones it DENIES (APMF_API::EquipCategory: Armor, Shield,
// Right, Left, Ammo, Light; default {All, 0} == v8). Each governed item COMPETES
// for one or more categories, mapped by ONE function -- Categorize() below --
// that the thunk (with the engine's resolved slot) and the enforce pass (with
// the declared hand) both call, so the two halves can never disagree on what a
// shield or a torch competes for. The verdict, in order: the script/console and
// player-menu exemptions FIRST (they sit above both masks); then `competes &
// denied` -> deny even if in-set; then `competes & owned` -> the v8 in-set test;
// else -> allow (`owned=0`: nothing the claim holds is at stake). The obs line
// carries `cat=<categories> owned=<0|1> black=<0|1> eslot=<none|R|L|E|hex>`.
//
// HOW IT KNOWS WHO ASKED. The thunk is entered with EquipObject's own frame still
// on the stack, so it reads EquipObject's CALLER's return slot at a per-site,
// per-runtime depth measured from the bytes (Docs/HOOK-SITE-COVERAGE.md, "equip
// sink"): AE EquipObject = 5 pushes + sub rsp,0x50 -> [rsp+0x80]; AE sibling
// 38893 = 3 pushes + 0x50 -> [rsp+0x70]; SE 37938 = push rdi + 0x50 ->
// [rsp+0x60]; SE 37937 = 3 pushes + 0x50 -> [rsp+0x70]. That address is turned
// into an Address-Library id (the greatest library offset <= the RVA, i.e. the
// containing function) and named from a small per-runtime path table; anything
// outside SkyrimSE.exe is `External(<module>)`, and an engine caller not in the
// table is `Unknown(<id>)` -- the first field build's probe criteria
// (Docs/INTEGRATION.md) require zero of those.
//
// THREADING. The thunk runs on whatever thread the engine equips on. It reads
// ONLY the lock-free RCU snapshot (ControlMap::TryGetEquipSet, FormID compares --
// never LookupByID, never client state; INVARIANTS #12), an immutable sorted
// offset table built once at install, and a small mutex-guarded dedupe/rate
// state touched only for claimed actors. spdlog is thread-safe.
//
// WHAT BREAKS. (1) The frame depths are per-site AND per-runtime; a new runtime
// needs them re-measured from its bytes, never assumed. (2) The install byte-
// verify is the collision guard: two patchers at one site is a refusal here,
// never a CTD -- do not weaken it to "warn and install anyway". (3) The TLS
// bracket is what lets APMF's own enforcement through; an enforcement path that
// forgets the bracket loses the attribution (its equips still pass, being in-set,
// but the probe can no longer tell them from an engine equip of the same item).
// (4) The runtime gate is EXACT-VERSION (1.6.1170 || 1.5.97), the same predicate
// core/CastClassify.cpp uses -- never IsAE()/IsSE(); a new build is gated by
// name, then its sites, offsets AND frame depths are re-measured from its bytes.
// ============================================================================

namespace apmf::equipsink {

    // kDataLoaded. VR-refused, install-once, INI-gated ([EquipAuthority]
    // bEquipAuthority, default 1). Byte-verifies all sites before writing ANY of
    // them; a single mismatch refuses the whole seat and logs each site's bytes.
    void Install();

    // True once both sites are patched (false when refused, disabled, or VR).
    bool Installed();

    // WHY Installed() is false, as a string literal ("" once installed; before
    // Install runs it reads "not yet installed ..."). Any thread. ControlMap uses
    // it to REFUSE a kIntent_EquipAuthority claim while the seat is down (ABI v8,
    // MFO wiring review SEV-2 F1): a client that turns its own equips off on a
    // successful claim must never be left holding a claim that enforces nothing.
    const char* NotInstalledReason();

    // Installed() AND the INI is not observe-only -- what APMF_API_v8::
    // IsEquipAuthorityEnforced reports. A claim's OWN kEquipAuth_ObserveOnly bit is
    // not consulted (the client that set it knows). Any thread.
    bool Enforcing();

    // Re-run the public-entry detour inspection (main thread, outside any engine
    // frame: plugin.cpp calls it at kPostLoadGame and kNewGame). A plugin later
    // in load order may detour EquipObject's entry after our kDataLoaded pass;
    // this logs ONLY when the verdict changes. No-op when the seat is not installed.
    void ReinspectEntries(const char* why);

    // RAII bracket for equips APMF ISSUES ITSELF (channels/EquipAuthority.cpp).
    // While one is alive on the current thread, every engine equip the sink sees
    // on that thread logs `tls=<depth>` -- the attribution that proves an equip
    // was APMF's. It is NOT a bypass: the verdict is the in-set test regardless.
    // Depth-counted so nesting is safe. Never copyable; never hold one across a
    // frame boundary.
    class ApmfEquipScope {
    public:
        ApmfEquipScope();
        ~ApmfEquipScope();
        ApmfEquipScope(const ApmfEquipScope&) = delete;
        ApmfEquipScope& operator=(const ApmfEquipScope&) = delete;
    };

    // Current thread's bracket depth (0 == not inside an APMF-issued equip).
    int ApmfDepth();

    // The INI switches, read once at Install (default 1 / 1 / 0). Either the
    // INI or the claim's own flag bits can set observe-only / deny-script.
    bool ObserveOnly();
    bool DenyScript();

    // The vanilla hand EQUP forms (Skyrim.esm DOBJ `RHEQ` -> 0x13F42 "RightHand",
    // `LHEQ` -> 0x13F43 "LeftHand"). Skyrim.esm is always load index 00, so the
    // runtime FormID is the file FormID; the engine's own InitItemImpl fills its
    // LHEQ/RHEQ default objects with exactly this lookup, so the slot the engine
    // calls "left" IS this form on both runtimes. Compared by FormID only here
    // (no lookup at the seat); channels/EquipAuthority.cpp resolves them by
    // LookupByID on the main thread for its own equip calls.
    inline constexpr RE::FormID kRightHandEquipSlot = 0x00013F42;
    inline constexpr RE::FormID kLeftHandEquipSlot  = 0x00013F43;

    // ABI v9: the ONE category map. Which APMF_API::EquipCategory bits `obj`
    // competes for when equipped into `slot` (the engine's resolved slot at the
    // seat; the DECLARED hand's EQUP in the enforce pass, nullptr for Default).
    // Pure member reads (form type, biped slot mask, weapon animation type, the
    // slot's FormID): safe on any thread, never a LookupByID (INVARIANTS #12).
    //   ARMO without the shield bit                    -> Armor
    //   ARMO with BipedObjectSlot::kShield             -> Shield | Left
    //   WEAP 2H sword / 2H axe / bow / crossbow        -> Right | Left
    //   WEAP one-handed (incl. staff), slot 0x13F43    -> Left
    //   WEAP one-handed (incl. staff), slot 0x13F42    -> Right
    //   WEAP one-handed, any other or no slot          -> Right | Left  (CONSERVATIVE
    //        on purpose: EitherHand/null means the engine has not picked yet, so
    //        the item competes for both until it has)
    //   AMMO                                           -> Ammo
    //   LIGH                                           -> Light | Left
    //   anything else (not a governed type) or null    -> 0
    std::uint32_t Categorize(const RE::TESBoundObject* obj, const RE::BGSEquipSlot* slot);

    // A readable form of a category mask for the log: `+`-joined names
    // ("Shield+Left"), "none" for 0. Writes into `buf` (at least 48 bytes) and
    // returns it; never allocates (the thunk calls it inside an engine frame).
    const char* CategoryNames(std::uint32_t mask, char* buf, std::size_t size);

}
