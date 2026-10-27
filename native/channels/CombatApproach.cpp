#include "PCH.h"
#include "core/Sources.h"   // Harbinger 1a: seat census
#include "core/FacetAttribution.h"   // Harbinger 1b: the engine's area ranked; yield + drift named
#include "channels/CombatApproach.h"
#include "channels/CombatSubstrate.h"   // S2a: a substrate is not a fight (Suppressing)
#include "core/Allowance.h"   // SeatVerified / DerivesFrom / RuntimeSupported (mit-3.7 F1, G1)
#include "core/Clock.h"
#include "core/BehaviorTreeCatalog.h"   // kLeaves: the Attack / Bash leaf vtables (S5)
#include "core/ControlMap.h"
#include "core/Hook.h"   // OnMainThread (review F6: the save-thread check)
#include "core/Log.h"
#include "core/Registry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

// Win32 INI reader, declared by hand (the PCH does not pull in <Windows.h>) -- the same
// one-line import channels/TargetPin.cpp and channels/CombatReentryDeny.cpp use.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* lpAppName, const char* lpKeyName, int nDefault, const char* lpFileName);

// ============================================================================
// Channel 24 -- COMBAT APPROACH (kIntent_CombatApproach, ABI v19, batch A release blocker).
// marth 2026-09-30, on a held heal whose recipient is occluded: "instead of a cap it should do
// loots moveto towards the actor." marth 2026-10-05: "the point is per facet, movement is one
// facet. it shouldnt drop any other actions."
//
// THE FACET. While the claim is in force Harbinger owns ONE thing: where the actor's in-combat
// movement is bounded to -- "within R of X". Attack selection, both hands' casts, the combat
// target, blocking, equip and the combat behaviour itself keep deciding as they would without
// the claim: every place the engine's ACTION side reads the combat area (S3 / S4 / S5 below) is
// answered with the engine's own area, never the bound.
//
// HOW THE ENGINE DECIDES IN-COMBAT MOVEMENT (disassembly of all three unpacked images,
// 2026-10-05; scratchpad agentlogs/apmf-combat-moveto.md has the listings and addresses).
//   * CombatBehaviorTreeCombat (AE builder 0x85F7D0) roots every NPC fight in "Combat Parallel"
//     = Parallel{ Movement link, Action link }. Movement and Action are the engine's OWN two
//     facets of combat, run side by side.
//   * CombatBehaviorTreeMovement (AE builder 0x8AD390): "Movement Parallel" { Check Unreachable
//     Target, Acquire Weapon, Acquire Movement Resource -> Movement Repeat -> "Movement Selector",
//     Dodge Threat }. The selector is a DynamicSelector<ConditionalChildSelector> that
//     re-selects every 0.25 s and takes the FIRST child whose value is non-zero (AE 0x85AEE0):
//     Essential Down > Flight > Flee > Hide > RETURN TO COMBAT AREA > Exit Water > Search >
//     Ranged Movement > Flanking Movement > Close Movement (constant 1, the default). Return To
//     Combat Area's value is NOT CombatController::IsActorInArea (AE 0x55AEC0): the current
//     combat area's vfunc 0x06 with the actor's position and bound radius.
//   * The CURRENT COMBAT AREA (CombatController runtime +0x28, AE abs +0x98, SE +0x90) is picked
//     by CombatController::UpdateAreas (AE 0x55BF50 / SE 0x5004E0 / 1.7.104 0x563DC0): each
//     active area's Update (vfunc 0x0B) when its timer is due, then the active+enabled area with
//     the HIGHEST priority (+0x28: standard 1, a package's HoldPosition 3). A priority pick, not
//     a weighted one. Inside the area, nine movement leaves clamp their destinations to it
//     (CombatController 0x55B340 -> vfunc 0x07) and the combat path search tests it (0x55B820).
//   * Every CombatController is built with ONE CombatAreaStandard (AE ctor 0x558070 -> 0x7FA490
//     -> AddArea 0x55C080): centred on the attacker, radius 4096 (more in some cells), updated
//     once a second, enabled when the actor is HIGH process and not fleeing.
//   So the in-combat movement GOAL is not a weighted choice: it is the current area, picked by
//   priority, and the movement half of the tree keeps the actor inside it. That makes it a
//   deny + substitute of ONE unweighted input, which the Harbinger rules allow.
//   (What the OOC road -- ch.19's package offer -- cannot do here: with a controller the Movement
//   half ALWAYS runs a movement leaf -- Close Movement is the constant default -- so it owns the
//   body; and a kIgnoreCombat package pauses the WHOLE tree (AE 0x6D7E80 -> 0x55A870 sets the
//   controller's ignoringCombat and stops the behaviour controller), dropping every facet. The
//   engine's own package-to-combat movement bridge IS the combat area: BGSProcedureHoldPosition
//   (vtable slot 12 -> AE 0x55B010) adds a CombatAreaHoldPosition.)
//
// THE SEATS (all write_vfunc, chaining -- INVARIANTS #17; no call-site patch):
//   S1 CombatAreaStandard vfunc 0x0B (Update), on the actor's combat thread inside UpdateAreas.
//      It first puts the engine's own geometry back if the area still carries the one S1 wrote
//      (so the engine ALWAYS computes from its own state -- its centre is sticky), runs the
//      original, records the engine's result as the VANILLA geometry, then -- for an actor whose
//      claim is live -- writes X's position, X's cell space and R into the same fields and makes
//      the area update 4x a second (period 0.25 s) while it carries the bound. The engine's own
//      priority pick then decides: a HoldPosition area (a package's force) still wins.
//   S2 CombatAreaStandard vfunc 0x05 (IsInside(WorldLocation*, extra)). Answers from the
//      VANILLA geometry while a deny bracket (S3/S4) is open on this thread, else chains.
//   S3 CombatTargetSelectorStandard vfunc 0x06 (SelectTarget): a deny BRACKET. The target score
//      subtracts a penalty for a candidate outside the current area (AE 0x84D66F, SE 0x7B6455);
//      inside the bracket S2 shows it the engine's own area, so the target facet sees no change.
//      (ch.20 hooks the same slot; both chain, either order.)
//   S4 the ten CombatInventoryItemMagicT<{Magic,Staff,Scroll,Potion,Shout},{Invisibility,
//      BoundItem}> vfunc 0x0F (CheckShouldEquip): a deny BRACKET. Their cast decision asks whether
//      the combat TARGET is inside the current area (AE helper 0x55B6D0 -> vfunc 0x05); inside the
//      bracket it sees the engine's own area, so those casts are decided exactly as without the
//      claim. (EquipGate hooks four of the same slots; both chain, either order.)
//   S5 CombatBehaviorAttack / CombatBehaviorBash act() (slot 0x02): deny BRACKETS (review F1 on
//      274d7e4). The attack pick rejects every attack whose end point is outside the current
//      area; inside the bracket it sees the engine's own area. (ActionGate hooks the same act()
//      slots; chains.) CheckUnreachableTarget is NOT bracketed (review R2-1 on acb832b): it is
//      part of the movement facet. Its update (AE 0x8AD220 -> 0x8A6660) skips the reachability
//      test for a target outside the area -- "outside my area, do not test" IS what a bound
//      means -- and a bracket cannot change the outcome anyway: the path request it builds
//      (0x8D7180, +0xBA |= 1) is validated later (slot 6 0x8C4ED0 -> 0x55B820 -> area slot 10
//      0x7F9D10) on the REAL current area, which confines the path to the R-disc and would mark
//      every target beyond R unreachable.
//   S6 CombatAreaStandard vfunc 0x06 (IsInside(NiPoint3*, extra)): OBSERVE-ONLY. When the point
//      is the claimed actor's own position (IsActorInArea passes &actor->data.location), the
//      engine's answer is recorded: that is the movement selector's own "outside -> return to
//      the area" decision, the principle-5 evidence that the bound is being acted on.
//   Every raw offset below was read off the engine's own code on 1.6.1170, 1.5.97 and 1.7.104
//   (identical); each seat checks the object's vtable is the Standard one before touching a
//   field (INVARIANTS #20 guards: kill-switch, self-check, per-call vtable identity).
//
// HARBINGER 1b (2026-10-12): ATTRIBUTION ON THE COMBAT FACET. (a) S1's substitution is decided
// against the source of the area it replaces -- the module hooked inside S1's seat if any (the
// census's displaced value), else Engine -- and the bound is written ONLY when the claim's priority
// is strictly above that source's (APMF_Sources.ini; defaults client 50 > unregistered 20, so no
// change unless the player raises it). Otherwise the engine's area stands that update (kSeatStood,
// reported as YIELDED). (b) A yield to a higher-priority area is named after the actor's running
// package's plugin (attribution only; the package still wins). (c) The DRIFT DETECTOR: an area
// whose geometry no longer matches what S1 wrote (same controller) was rewritten outside every
// Harbinger seat -- Unattributed (alert + line), not undone.
//
// DENY-COMPLETENESS (principle 2). The facet is "the in-combat movement bound". Competing
// sources: the engine's own standard area -- SUBSTITUTED while the claim stands; a package's
// HoldPosition area -- WINS by the engine's own priority (they force, they win); the engine's
// forced reselect at flee start (AE 33258) -- restores the engine's pick for the rest of that
// tree step only, and Flee outranks Return To Combat Area anyway. Facets the area switches on
// that are NOT movement are DENIED -- every engine reader of the area classified by tree half in
// Docs/DENY-COMPLETENESS-AUDIT.md row 24: the attack / bash pick (S5),
// the target score penalty (S3), the invisibility / bound-item cast test (S4), and (APMF-B57, marth
// 2026-10-11) the Acquire Weapon search (CombatBehaviorFindWeapon act, S5): a disarmed actor's search
// for a dropped weapon is EXEMPT from the bound. The path to the chosen weapon stays part of the
// Movement half and is still confined. On release the engine's next update (<= 0.25 s) recomputes the area from
// its own state; an area the engine DEACTIVATED (its own path back to X failed) is put back and
// re-activated by S3 on the actor's own combat thread (review F3).
//
// SAVE. The current area is saved by its INDEX in the controller's areas array (AE 0x559DB0),
// and the standard area itself is saved with its fields, so RestoreBeforeSave (kSaveGame, before
// the game writes) puts the vanilla geometry back first; the next update re-applies the bound.
//
// THREADING. S1/S2/S3/S4/S6 run on BSJobs combat threads (one actor per job, several in
// parallel). They read: the claims table (g_entryMx, shared), the touched-area table (g_areaMx;
// S1 takes it unique, S2/S6 shared) and their own thread_local bracket depth. They never look up
// a form (X is resolved to a position on the game thread by Poll), and the only engine call is
// the attacker handle lookup (ActorHandle::get, the ch.23 seat precedent). Poll, Engage/Release,
// GetState and RestoreBeforeSave run on the game thread (GetState: any thread, under the lock).
// ============================================================================

namespace {

    using apmf::log::Hex;

    constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

    // ---- CombatArea layout (identical on 1.6.1170 / 1.5.97 / 1.7.104; CombatAreaRadius
    // vfuncs 0x05/0x06/0x08/0x09 and CombatAreaStandard 0x0B read and write exactly these). ----
    constexpr std::size_t kAreaCtl      = 0x10;   // CombatController*
    constexpr std::size_t kAreaTimer    = 0x20;   // float: last update (game seconds)
    constexpr std::size_t kAreaPeriod   = 0x24;   // float: update period (standard: 1.0)
    constexpr std::size_t kAreaPriority = 0x28;   // int32: the UpdateAreas pick key
    constexpr std::size_t kAreaActive   = 0x2C;   // uint8: Update runs, selectable
    constexpr std::size_t kAreaEnabled  = 0x2D;   // uint8: selectable (standard: high process, not fleeing)
    constexpr std::size_t kAreaCenter   = 0x30;   // NiPoint3 (a WorldLocation: +0x10 = the space)
    constexpr std::size_t kAreaSpace    = 0x40;   // TESObjectCELL* (interior) / TESWorldSpace* (exterior)
    constexpr std::size_t kAreaRadius   = 0x48;   // float
    // WorldLocation (the IsInside(WorldLocation*) argument; AE 0x2EA5E0 builds it): pos +0, space +0x10.
    constexpr std::size_t kLocSpace = 0x10;

    constexpr std::size_t kSlotIsInsideLoc = 0x05;
    constexpr std::size_t kSlotIsInsidePos = 0x06;
    constexpr std::size_t kSlotUpdate      = 0x0B;
    constexpr std::size_t kSlotSelect      = 0x06;   // CombatTargetSelectorStandard::SelectTarget
    constexpr std::size_t kSlotShouldEquip = 0x0F;   // CombatInventoryItem::CheckShouldEquip

    constexpr float         kBoundPeriod  = 0.25f;   // our refresh while the bound is carried
    constexpr std::uint64_t kPollMs       = 100;
    // The stall, verdict and purge windows run on the WORLD-TICK clock below, never the wall
    // clock (review F2 on 274d7e4): the seats run on game time, and Poll does not run while a
    // menu pauses the game, so a wall clock would end a live claim (and purge its record before
    // S1 can restore the engine geometry) after any pause longer than the window.
    constexpr float         kLeashClampMargin = 16.0f;   // APMF-B55: how far inside the ch.23 leash a clamped goal sits
    constexpr std::uint64_t kStallMs      = 3000;    // 12 x the 0.25 s period: a FLOOR (principle 9)
    constexpr std::uint64_t kVerdictMs    = 1500;    // an engine inside/outside answer older than this is stale
    constexpr std::uint64_t kPurgeMs      = 5000;    // a record S1 has not rewritten for this long is dead
    constexpr std::uint64_t kPurgeDeadMs  = 60000;   // ...unless it still waits for its F3 reactivation
    constexpr std::uint64_t kTickClampMs  = 100;     // one Poll-to-Poll gap counts at most this much world time
    constexpr std::uint64_t kLineMs       = 3000;    // per-actor status line
    constexpr std::uint64_t kHeartbeatMs  = 30000;

    template <class T>
    T& At(void* a_obj, std::size_t a_off) {
        return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(a_obj) + a_off);
    }

    struct Geo {
        float       x = 0.0f, y = 0.0f, z = 0.0f;
        const void* space  = nullptr;
        float       radius = 0.0f;
    };

    Geo ReadGeo(void* a_area) {
        Geo g;
        g.x      = At<float>(a_area, kAreaCenter + 0);
        g.y      = At<float>(a_area, kAreaCenter + 4);
        g.z      = At<float>(a_area, kAreaCenter + 8);
        g.space  = At<const void*>(a_area, kAreaSpace);
        g.radius = At<float>(a_area, kAreaRadius);
        return g;
    }

    void WriteGeo(void* a_area, const Geo& g) {
        At<float>(a_area, kAreaCenter + 0) = g.x;
        At<float>(a_area, kAreaCenter + 4) = g.y;
        At<float>(a_area, kAreaCenter + 8) = g.z;
        At<const void*>(a_area, kAreaSpace) = g.space;
        At<float>(a_area, kAreaRadius)      = g.radius;
    }

    // Bit-exact: "does this area still carry exactly what S1 wrote".
    bool SameGeo(const Geo& a, const Geo& b) {
        return std::memcmp(&a.x, &b.x, sizeof(float)) == 0 && std::memcmp(&a.y, &b.y, sizeof(float)) == 0 &&
               std::memcmp(&a.z, &b.z, sizeof(float)) == 0 && a.space == b.space &&
               std::memcmp(&a.radius, &b.radius, sizeof(float)) == 0;
    }

    // The engine's own "space" of a reference, exactly as AE 0x2EA5E0 (GetWorldLocation) computes
    // it for a reference with a parent cell: an interior cell is its own space; an exterior cell's
    // space is its worldspace (or the cell when it has none). No parent cell -> nullptr (refused).
    const void* SpaceOf(const RE::TESObjectREFR* a_ref) {
        auto* cell = a_ref ? a_ref->GetParentCell() : nullptr;
        if (!cell) return nullptr;
        if (cell->IsInteriorCell()) return cell;
        auto* ws = cell->GetRuntimeData().worldSpace;
        return ws ? static_cast<const void*>(ws) : static_cast<const void*>(cell);
    }

    // ---- install state ----
    std::atomic<bool>        g_installed{ false };
    std::atomic<bool>        g_installTried{ false };
    std::atomic<const char*> g_notInstalledReason{ "before kDataLoaded (the seats install there)" };
    std::uintptr_t           g_vtStd = 0;
    std::uintptr_t           g_vtSel = 0;   // CombatTargetSelectorStandard (S3's per-call identity check)

    using Update_t      = void (*)(void*);
    using IsInsideLoc_t = bool (*)(void*, const void*, float);
    using IsInsidePos_t = bool (*)(void*, const RE::NiPoint3*, float);
    using Select_t      = std::uint32_t* (*)(void*, std::uint32_t*);
    using ShouldEquip_t = bool (*)(void*, void*);

    std::atomic<std::uintptr_t> g_origUpdate{ 0 };
    std::atomic<std::uintptr_t> g_origIsInsideLoc{ 0 };
    std::atomic<std::uintptr_t> g_origIsInsidePos{ 0 };
    std::atomic<std::uintptr_t> g_origSelect{ 0 };
    // vtable -> original CheckShouldEquip, written once at Install (before g_installed), read-only after.
    std::unordered_map<std::uintptr_t, std::uintptr_t> g_origShouldEquip;

    // ---- the claims table (one entry per actor that ever held a ch.24 claim this session) ----
    struct Entry {
        // Declared + decided on the GAME thread, under g_entryMx (unique). Seats copy under shared.
        RE::FormID       target   = 0;
        float            radius   = 0.0f;
        std::uint32_t    flags    = 0;
        APMF_API::Handle owner    = APMF_API::kInvalidHandle;
        bool             live     = false;   // the seats apply `want` while true
        Geo              want{};
        bool             released = false;   // the claim is gone (state is history)
        bool             ending   = false;   // Harbinger enqueued its release
        bool             wasApplied = false;
        std::uint32_t    state    = APMF_API::kApproachState_Waiting;
        std::uint64_t    stateSinceMs = 0;
        std::uint32_t    seq      = 0;
        float            distance = -1.0f;
        std::uint64_t    claimMs  = 0;       // Engage / OnOwnerChanged time
        std::uint64_t    inCombatSinceMs = 0;
        std::uint64_t    lastLineMs = 0;
        // Written by the seats (combat threads), read by Poll.
        std::atomic<std::uint32_t> applied{ 0 };
        std::atomic<std::uint64_t> lastSeenMs{ 0 };     // S1 ran for this actor's standard area
        std::atomic<std::uint64_t> lastApplyMs{ 0 };    // S1 wrote the bound
        std::atomic<std::uint32_t> seatFlags{ 0 };      // kSeat* below, as of the last S1 run
        std::atomic<std::uint32_t> inside{ 0 };         // 0 unseen, 1 outside, 2 inside (S6)
        std::atomic<std::uint64_t> insideMs{ 0 };
        std::atomic<std::uint32_t> outsideCount{ 0 };   // S6 "outside" answers for the bound
    };
    constexpr std::uint32_t kSeatYielded  = 1u << 0;   // a higher-priority area was current
    constexpr std::uint32_t kSeatDisabled = 1u << 1;   // the engine disabled the standard area
    constexpr std::uint32_t kSeatCurrent  = 1u << 2;   // the standard area was the current one
    constexpr std::uint32_t kSeatStood    = 1u << 3;   // Harbinger 1b: the claim does not outrank the source
                                                       //   of the engine's own area (APMF_Sources.ini): not written

    std::shared_mutex                                       g_entryMx;
    std::unordered_map<RE::FormID, std::unique_ptr<Entry>> g_entries;
    std::atomic<std::size_t>                                g_liveCount{ 0 };   // entries with live == true
    std::atomic<std::uint32_t>                              g_seq{ 0 };

    // ---- HARBINGER-INTERNAL BOUNDS (2026-10-12, held engagement + leash return) ----
    // Not client claims. channels/CombatEntry.cpp (a held engagement on a bleeding-out target) and
    // core/ActionGate.cpp (ch.23's leash return) ask S1 to carry "within R of X" for an actor, through
    // exactly the same write + record + deny brackets as a ch.24 claim. A LIVE ch.24 claim on the same
    // actor always wins: S1 consults this map only when the actor has none (the client's declared
    // area is the client's choice). Written on the GAME thread under g_intMx (unique); S1 copies under
    // shared. Lock order: g_entryMx is released before g_intMx is taken; g_intMx before g_areaMx.
    struct Internal {
        Geo              want{};
        APMF_API::Handle owner = APMF_API::kInvalidHandle;   // the claim this bound implements (1b ranking)
        const char*      who   = "";                         // a string literal: "held engagement" / "leash return"
        std::atomic<std::uint32_t> applied{ 0 };
        std::atomic<std::uint64_t> lastApplyMs{ 0 };          // WORLD time S1 last wrote it
        std::atomic<std::uint32_t> stood{ 0 };                // 1b: not written (the source outranks the owner)
    };
    std::shared_mutex                                        g_intMx;
    std::unordered_map<RE::FormID, std::unique_ptr<Internal>> g_internal;
    std::atomic<std::size_t>                                 g_intCount{ 0 };
    std::atomic<std::uint64_t>                               g_s1Internal{ 0 };   // RULE C: internal bounds written

    // ---- the touched-area table: areas S1 has written a bound into ----
    struct Touched {
        RE::FormID  actor   = 0;
        void*       ctl     = nullptr;
        const void* actorLoc = nullptr;   // &actor->data.location, an IDENTITY for S6 (never dereferenced)
        Geo         vanilla{};
        float       vanillaPeriod = 1.0f;
        Geo         ours{};
        std::uint64_t ms = 0;             // WORLD time S1 wrote it (it rewrites it every 0.25 s while carrying the bound)
        bool        reactivate = false;   // F3: the claim is over; S3 must put the engine's area back (geometry,
                                          //   period and, if the engine's failed path deactivated it, +0x2C)
    };
    std::shared_mutex                         g_areaMx;
    std::unordered_map<void*, Touched>        g_touched;
    std::atomic<std::size_t>                  g_touchedCount{ 0 };
    std::atomic<std::size_t> g_reactivateCount{ 0 };   // records with reactivate set (S3's pre-gate)

    // F3 (review on 274d7e4): put the engine's own area back into one recorded area -- the vanilla
    // geometry and period, and +0x2C (active) when the engine's own Return To Combat Area path to X
    // failed and cleared it (RTCA update AE 0x8AD2C0, path status 5): UpdateAreas never updates or
    // selects an inactive area, so without this the actor would fight the rest of that fight with no
    // standard area. Only an area that still carries exactly what S1 wrote is written. Caller holds
    // g_areaMx (unique) and has proven the area alive.
    bool PutBack(void* a_area, const Touched& t) {
        if (*reinterpret_cast<const std::uintptr_t*>(a_area) != g_vtStd || !SameGeo(ReadGeo(a_area), t.ours))
            return false;
        WriteGeo(a_area, t.vanilla);
        At<float>(a_area, kAreaPeriod) = t.vanillaPeriod;
        if (t.reactivate) At<std::uint8_t>(a_area, kAreaActive) = 1;   // only a FINISHED claim's area
        return true;
    }

    // Mark every record of `a_actor` for F3 (End / Release; game thread).
    void MarkReactivate(RE::FormID a_actor) {
        std::unique_lock lk(g_areaMx);
        for (auto& [area, t] : g_touched)
            if (t.actor == a_actor && !t.reactivate) {
                t.reactivate = true;
                g_reactivateCount.fetch_add(1, std::memory_order_relaxed);
            }
    }

    thread_local int t_view = 0;   // > 0: a deny bracket (S3/S4) is open on this thread

    // ---- RULE C counters (printed even at zero) ----
    std::atomic<std::uint64_t> g_s1Seen{ 0 }, g_s1Applied{ 0 }, g_s1Restored{ 0 }, g_s1Yielded{ 0 },
        g_s1Disabled{ 0 }, g_s2Vanilla{ 0 }, g_s3Brackets{ 0 }, g_s4Brackets{ 0 }, g_s6Verdicts{ 0 },
        g_s6Outside{ 0 }, g_ended{ 0 }, g_s5Brackets{ 0 }, g_s3PutBack{ 0 }, g_s1Stood{ 0 }, g_s1Drift{ 0 };
    std::atomic<bool>          g_s1Observed{ false };
    // Harbinger 1b: the source whose answer S1 substitutes -- the module hooked INSIDE S1's seat (it
    // wraps the engine's Update, so its result is the last word below Harbinger), else Engine. Set
    // once at Install, after the seat is registered, before g_installed.
    std::atomic<int>           g_s1Src{ apmf::sources::kEngineSource };

    // WORLD-TICK CLOCK (channels/Idle.cpp / channels/Travel.cpp precedent). Advanced by Poll on
    // the game thread, every call, by the wall gap CLAMPED to kTickClampMs: a pause, a load
    // screen or a hitch adds at most one clamp. Read by the seats (combat threads) lock-free.
    // Starts at 1 so 0 can mean "never".
    std::atomic<std::uint64_t> g_worldMs{ 1 };
    std::uint64_t              g_lastTickMs = 0;   // game thread only
    std::uint64_t WorldNow() { return g_worldMs.load(std::memory_order_relaxed); }
    std::uint64_t              g_lastHeartbeatMs = 0;

    void SetState(Entry& e, std::uint32_t s, std::uint64_t now) {
        if (e.state == s) return;
        e.state        = s;
        e.stateSinceMs = now;
        e.seq          = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    void SetLive(Entry& e, bool live) {
        if (e.live == live) return;
        e.live = live;
        if (live) g_liveCount.fetch_add(1, std::memory_order_relaxed);
        else g_liveCount.fetch_sub(1, std::memory_order_relaxed);
    }

    const char* StateName(std::uint32_t s) {
        switch (s) {
        case APMF_API::kApproachState_None:          return "none";
        case APMF_API::kApproachState_Waiting:       return "WAITING (not in combat)";
        case APMF_API::kApproachState_Approaching:   return "APPROACHING";
        case APMF_API::kApproachState_Holding:       return "HOLDING";
        case APMF_API::kApproachState_Yielded:       return "YIELDED (a package hold-position area owns the bound)";
        case APMF_API::kApproachState_Disabled:      return "DISABLED (fleeing or not high process)";
        case APMF_API::kApproachState_Leashed:       return "LEASHED (a ch.23 leash excludes the goal)";
        case APMF_API::kApproachState_Arrived:       return "ARRIVED";
        case APMF_API::kApproachState_TargetLost:    return "TARGET LOST";
        case APMF_API::kApproachState_CombatEnded:   return "COMBAT ENDED";
        case APMF_API::kApproachState_EngineDropped: return "ENGINE DROPPED THE BOUND";
        case APMF_API::kApproachState_ActorGone:     return "ACTOR GONE";
        case APMF_API::kApproachState_Released:      return "RELEASED";
        case APMF_API::kApproachState_Refused:       return "REFUSED (invalid re-point)";
        default:                                     return "?";
        }
    }

    // S1's internal-bound half (see Internal above): the actor has NO live ch.24 claim. Runs after the
    // engine's own Update, on the same combat thread, with the same checks a claim gets: the area must be
    // enabled, and Harbinger 1b ranks the bound's owner claim against the area's source.
    void ApplyInternal(void* a_area, void* a_ctl, RE::Actor* a_actor, RE::FormID a_fid) {
        if (g_intCount.load(std::memory_order_relaxed) == 0) return;
        std::shared_lock ilk(g_intMx);
        const auto it = g_internal.find(a_fid);
        if (it == g_internal.end()) return;
        Internal& in = *it->second;
        if (At<std::uint8_t>(a_area, kAreaEnabled) == 0) return;   // not selectable: carrying it changes nothing
        apmf::facetattr::Verdict v;
        try {
            v = apmf::facetattr::Decide(apmf::sources::Facet::kCombat, in.owner,
                                        g_s1Src.load(std::memory_order_relaxed), a_fid, "");
        } catch (...) {
            v.enforce = true;   // the decision could not be recorded: the pre-1b behaviour
        }
        if (!v.enforce) {
            in.stood.fetch_add(1, std::memory_order_relaxed);
            g_s1Stood.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::uint64_t now = WorldNow();
        {
            std::unique_lock lk(g_areaMx);
            Touched t;
            t.actor         = a_fid;
            t.ctl           = a_ctl;
            t.actorLoc      = &a_actor->data.location;
            t.vanilla       = ReadGeo(a_area);
            t.vanillaPeriod = At<float>(a_area, kAreaPeriod);
            t.ours          = in.want;
            t.ms            = now;
            WriteGeo(a_area, t.ours);
            At<float>(a_area, kAreaPeriod) = kBoundPeriod;
            g_touched[a_area] = t;   // S1's step 1 erased any previous record of this area
            g_touchedCount.store(g_touched.size(), std::memory_order_relaxed);
        }
        in.applied.fetch_add(1, std::memory_order_relaxed);
        in.lastApplyMs.store(now, std::memory_order_relaxed);
        g_s1Internal.fetch_add(1, std::memory_order_relaxed);
    }

    // ======================================================================
    // S1 -- CombatAreaStandard::Update (vfunc 0x0B). Combat thread, inside UpdateAreas.
    // ======================================================================
    void UpdateThunk(void* a_area) {
        const auto orig = reinterpret_cast<Update_t>(g_origUpdate.load(std::memory_order_relaxed));
        g_s1Seen.fetch_add(1, std::memory_order_relaxed);
        if (!g_s1Observed.exchange(true, std::memory_order_relaxed)) {
            spdlog::info("[ch.24] seat OBSERVED: CombatAreaStandard::Update (vfunc 0x0B) ran on a combat thread "
                         "(first of the session; principle 5).");
        }
        if (g_liveCount.load(std::memory_order_relaxed) == 0 && g_touchedCount.load(std::memory_order_relaxed) == 0 &&
            g_intCount.load(std::memory_order_relaxed) == 0)
            return orig(a_area);
        if (!a_area || *reinterpret_cast<const std::uintptr_t*>(a_area) != g_vtStd) return orig(a_area);

        void* const ctl = At<void*>(a_area, kAreaCtl);

        // 1) The engine computes from ITS OWN state: undo what S1 wrote last time, if it is still there.
        RE::FormID driftActor = 0;   // Harbinger 1b: the bound was rewritten by something outside this seat
        if (g_touchedCount.load(std::memory_order_relaxed) != 0) {
            std::unique_lock lk(g_areaMx);
            if (const auto it = g_touched.find(a_area); it != g_touched.end()) {
                if (it->second.ctl == ctl && SameGeo(ReadGeo(a_area), it->second.ours)) {
                    WriteGeo(a_area, it->second.vanilla);
                    At<float>(a_area, kAreaPeriod) = it->second.vanillaPeriod;
                    g_s1Restored.fetch_add(1, std::memory_order_relaxed);
                } else if (it->second.ctl == ctl) {
                    driftActor = it->second.actor;   // same controller, not what S1 wrote: an outside write
                }
                if (it->second.reactivate) g_reactivateCount.fetch_sub(1, std::memory_order_relaxed);
                g_touched.erase(it);
                g_touchedCount.store(g_touched.size(), std::memory_order_relaxed);
            }
        }
        if (driftActor != 0) {
            // THE DRIFT DETECTOR (plan row "Combat approach (24)"). Every engine write to the standard
            // area's geometry goes through Update (this seat) and every APMF write is recorded, so a
            // record whose area no longer carries what S1 wrote was written by code no Harbinger seat
            // sees. It is NOT undone (the engine's Update below computes from what is there now); it
            // is named Unattributed (alert + rate-limited line), the degrade rule.
            g_s1Drift.fetch_add(1, std::memory_order_relaxed);
            try {
                apmf::sources::Unattributed(apmf::sources::Facet::kCombat, driftActor,
                                            "ch.24: the standard combat area carrying the bound was rewritten "
                                            "outside its Update seat (not undone)");
            } catch (...) {
            }
        }

        orig(a_area);   // the engine's own update: enabled flag, centre, radius, period

        if (!ctl || (g_liveCount.load(std::memory_order_relaxed) == 0 && g_intCount.load(std::memory_order_relaxed) == 0))
            return;
        auto* const cc    = static_cast<RE::CombatController*>(ctl);
        const auto  actor = cc->attackerHandle.get();
        if (!actor) return;
        const RE::FormID fid = actor->GetFormID();

        std::shared_lock elk(g_entryMx);
        const auto eit = g_entries.find(fid);
        if (eit == g_entries.end() || !eit->second->live) {
            elk.unlock();   // lock order: never g_entryMx and g_intMx together
            ApplyInternal(a_area, ctl, actor.get(), fid);
            return;
        }
        Entry& e = *eit->second;
        const std::uint64_t now = WorldNow();
        e.lastSeenMs.store(now, std::memory_order_relaxed);

        std::uint32_t sf = 0;
        void* const   cur = cc->GetRuntimeData().currentArea;
        if (cur == a_area) sf |= kSeatCurrent;
        else if (cur && At<std::int32_t>(cur, kAreaPriority) > At<std::int32_t>(a_area, kAreaPriority))
            sf |= kSeatYielded;
        const bool enabled = At<std::uint8_t>(a_area, kAreaEnabled) != 0;
        if (!enabled) sf |= kSeatDisabled;
        e.seatFlags.store(sf, std::memory_order_relaxed);
        if (sf & kSeatYielded) g_s1Yielded.fetch_add(1, std::memory_order_relaxed);
        if (!enabled) {   // not selectable at all: carrying the bound would change nothing
            g_s1Disabled.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Harbinger 1b: the engine's own area (or a hook inside this seat) is the source the bound
        // substitutes. The substitution stands only when the claim outranks it on the Combat facet;
        // otherwise the engine's area stands untouched this update (state YIELDED, logged).
        {
            apmf::facetattr::Verdict v;
            try {
                v = apmf::facetattr::Decide(apmf::sources::Facet::kCombat, e.owner,
                                            g_s1Src.load(std::memory_order_relaxed), fid, "");
            } catch (...) {
                v.enforce = true;   // the decision could not be recorded: keep the pre-1b behaviour
            }
            if (!v.enforce) {
                e.seatFlags.store(sf | kSeatStood, std::memory_order_relaxed);
                g_s1Stood.fetch_add(1, std::memory_order_relaxed);
                if (apmf::facetattr::LineDue(apmf::sources::Facet::kCombat, fid, 24u << 8 | 1u)) {
                    spdlog::info("[ch.24] 0x{} the bound is NOT written: claim h={} ({} priority {}) does not "
                                 "outrank {} (priority {}) on the Combat facet, so the engine's own combat area "
                                 "stands (APMF_Sources.ini). Logged at most once per 5 s per actor.",
                                 Hex(fid), e.owner, apmf::facetattr::NameOr(v.client, "an internal claim"),
                                 v.claimPrio, apmf::sources::Name(v.src), v.srcPrio);
                }
                return;
            }
        }

        // Write + record under the area lock (lock order: entries, then areas -- nothing takes them
        // the other way round), so RestoreBeforeSave and S2 never see a bound without its record.
        {
            std::unique_lock lk(g_areaMx);
            Touched t;
            t.actor         = fid;
            t.ctl           = ctl;
            t.actorLoc      = &actor->data.location;
            t.vanilla       = ReadGeo(a_area);
            t.vanillaPeriod = At<float>(a_area, kAreaPeriod);
            t.ours          = e.want;
            t.ms            = now;
            WriteGeo(a_area, t.ours);
            At<float>(a_area, kAreaPeriod) = kBoundPeriod;
            g_touched[a_area] = t;   // step 1 erased any previous record of this area
            g_touchedCount.store(g_touched.size(), std::memory_order_relaxed);
        }
        e.applied.fetch_add(1, std::memory_order_relaxed);
        e.lastApplyMs.store(now, std::memory_order_relaxed);
        g_s1Applied.fetch_add(1, std::memory_order_relaxed);
    }

    // ======================================================================
    // S2 -- CombatAreaStandard::IsInside(WorldLocation*, extra) (vfunc 0x05).
    // Inside a deny bracket, a bound-carrying area answers with the engine's own geometry:
    // exactly CombatAreaRadius 0x05 -> 0x06 (AE 0x7F9B90 / 0x7F9B40): same space, then
    // (radius + extra)^2 > |pos - centre|^2.
    // ======================================================================
    bool IsInsideLocThunk(void* a_area, const void* a_loc, float a_extra) {
        const auto orig = reinterpret_cast<IsInsideLoc_t>(g_origIsInsideLoc.load(std::memory_order_relaxed));
        if (t_view > 0 && a_loc && g_touchedCount.load(std::memory_order_relaxed) != 0 &&
            *reinterpret_cast<const std::uintptr_t*>(a_area) == g_vtStd) {
            Geo  v{};
            bool have = false;
            {
                std::shared_lock lk(g_areaMx);
                if (const auto it = g_touched.find(a_area); it != g_touched.end() &&
                                                            it->second.ctl == At<void*>(a_area, kAreaCtl) &&
                                                            SameGeo(ReadGeo(a_area), it->second.ours)) {
                    v    = it->second.vanilla;
                    have = true;
                }
            }
            if (have) {
                g_s2Vanilla.fetch_add(1, std::memory_order_relaxed);
                if (*reinterpret_cast<const void* const*>(reinterpret_cast<std::uintptr_t>(a_loc) + kLocSpace) !=
                    v.space)
                    return false;
                const auto* p  = static_cast<const float*>(a_loc);
                const float dy = p[1] - v.y, dx = p[0] - v.x, dz = p[2] - v.z;
                const float r  = a_extra + v.radius;
                return r * r > dy * dy + dx * dx + dz * dz;
            }
        }
        return orig(a_area, a_loc, a_extra);
    }

    // ======================================================================
    // S6 -- CombatAreaStandard::IsInside(NiPoint3*, extra) (vfunc 0x06). OBSERVE-ONLY.
    // ======================================================================
    bool IsInsidePosThunk(void* a_area, const RE::NiPoint3* a_pos, float a_extra) {
        const auto orig   = reinterpret_cast<IsInsidePos_t>(g_origIsInsidePos.load(std::memory_order_relaxed));
        const bool answer = orig(a_area, a_pos, a_extra);
        if (g_touchedCount.load(std::memory_order_relaxed) == 0 || t_view > 0) return answer;
        RE::FormID fid = 0;
        {
            std::shared_lock lk(g_areaMx);
            if (const auto it = g_touched.find(a_area);
                it != g_touched.end() && it->second.actorLoc == static_cast<const void*>(a_pos))
                fid = it->second.actor;
        }
        if (fid == 0) return answer;   // not the actor's own position: another consumer
        g_s6Verdicts.fetch_add(1, std::memory_order_relaxed);
        if (!answer) g_s6Outside.fetch_add(1, std::memory_order_relaxed);
        std::shared_lock elk(g_entryMx);
        if (const auto eit = g_entries.find(fid); eit != g_entries.end()) {
            eit->second->inside.store(answer ? 2u : 1u, std::memory_order_relaxed);
            eit->second->insideMs.store(WorldNow(), std::memory_order_relaxed);
            if (!answer) eit->second->outsideCount.fetch_add(1, std::memory_order_relaxed);
        }
        return answer;
    }

    // ======================================================================
    // S3 / S4 -- the deny brackets.
    // ======================================================================
    struct ViewBracket {
        ViewBracket() { ++t_view; }
        ~ViewBracket() { --t_view; }
        ViewBracket(const ViewBracket&)            = delete;
        ViewBracket& operator=(const ViewBracket&) = delete;
    };

    // CombatTargetSelectorStandard +0x10 = its CombatController (both constructors on all three builds;
    // channels/TargetPin.cpp reads it under the same per-call vtable-identity guard).
    constexpr std::size_t kSelectorCtl = 0x10;

    // F3: on the actor's OWN combat thread (the selector runs inside its controller's update), put the
    // engine's area back into every finished record of this controller -- the only safe place to write
    // an area S1 can no longer reach (an inactive area gets no Update call).
    void ReactivateFor(void* a_self) {
        if (*reinterpret_cast<const std::uintptr_t*>(a_self) != g_vtSel) return;
        void* const ctl = At<void*>(a_self, kSelectorCtl);
        if (!ctl) return;
        std::unique_lock lk(g_areaMx);
        for (auto it = g_touched.begin(); it != g_touched.end();) {
            if (!it->second.reactivate || it->second.ctl != ctl) { ++it; continue; }
            bool member = false;   // proven alive: in this (running) controller's own areas array
            for (auto* a : static_cast<RE::CombatController*>(ctl)->GetRuntimeData().areas)
                if (static_cast<void*>(a) == it->first) { member = true; break; }
            if (member && PutBack(it->first, it->second)) g_s3PutBack.fetch_add(1, std::memory_order_relaxed);
            g_reactivateCount.fetch_sub(1, std::memory_order_relaxed);
            it = g_touched.erase(it);
        }
        g_touchedCount.store(g_touched.size(), std::memory_order_relaxed);
    }

    std::uint32_t* SelectThunk(void* a_self, std::uint32_t* a_out) {
        const auto orig = reinterpret_cast<Select_t>(g_origSelect.load(std::memory_order_relaxed));
        if (a_self && g_reactivateCount.load(std::memory_order_relaxed) != 0) ReactivateFor(a_self);
        if (g_touchedCount.load(std::memory_order_relaxed) == 0) return orig(a_self, a_out);
        g_s3Brackets.fetch_add(1, std::memory_order_relaxed);
        ViewBracket b;
        return orig(a_self, a_out);
    }

    bool ShouldEquipThunk(void* a_item, void* a_ctl) {
        const auto vt = a_item ? *reinterpret_cast<const std::uintptr_t*>(a_item) : 0;
        const auto it = g_origShouldEquip.find(vt);
        if (it == g_origShouldEquip.end()) return false;   // foreign object: the benign default (#17)
        const auto orig = reinterpret_cast<ShouldEquip_t>(it->second);
        if (g_touchedCount.load(std::memory_order_relaxed) == 0) return orig(a_item, a_ctl);
        g_s4Brackets.fetch_add(1, std::memory_order_relaxed);
        ViewBracket b;
        return orig(a_item, a_ctl);
    }

    // ======================================================================
    // S5 -- deny brackets on the ACTION half's area consumers (review F1 / F4 on 274d7e4).
    //   CombatBehaviorAttack and CombatBehaviorBash act() (slot 0x02; AE 0x8A5230 / 0x86C4E0,
    //   SE 0x80E9A0 / 0x7D5350, 1.7.104 0x8BA700 / 0x881720) run the attack pick (AE 0x8A28B0),
    //   which rejects every attack whose END POINT is outside the current area (AE 0x8A2EE0 ->
    //   IsInCombatArea at 0x8A3186): with the bound around X, a melee actor outside it would
    //   attack nothing. Inside the bracket it sees the engine's own area. (CheckUnreachableTarget is
    //   deliberately NOT bracketed: review R2-1, see the header.)
    // ======================================================================
    using NodeAct_t    = void* (*)(void*, void*);   // apmf::cbt::Act_t (slot 0x02)
    std::atomic<std::uintptr_t> g_vtAttack{ 0 }, g_vtBash{ 0 }, g_vtFind{ 0 };
    std::atomic<std::uintptr_t> g_origAttackAct{ 0 }, g_origBashAct{ 0 }, g_origFindAct{ 0 };
    std::atomic<std::uint64_t>  g_s5FindBrackets{ 0 };   // APMF-B57: CombatBehaviorFindWeapon act() brackets

    // CombatBehaviorFindWeapon act() (slot 0x02; AE 0x864040 -> tail-jumps 0x8610C0, which calls IsInCombatArea
    // 0x55B310 twice; SE 0x7CCFB0 -> 0x7C9A40, same shape) is the Acquire Weapon link (APMF-B57, marth 2026-10-11:
    // "a disarmed follower's weapon search is exempt from the heal-approach bound"). Inside the bracket the
    // search sees the engine's own area, so a disarmed actor considers dropped weapons wherever the engine would.
    void* LeafActThunk(void* a_this, void* a_control) {
        const auto vt   = a_this ? *reinterpret_cast<const std::uintptr_t*>(a_this) : 0;
        const bool isFind = vt != 0 && vt == g_vtFind.load(std::memory_order_relaxed);
        const auto orig = reinterpret_cast<NodeAct_t>(
            (isFind ? g_origFindAct
                    : vt == g_vtBash.load(std::memory_order_relaxed) ? g_origBashAct : g_origAttackAct)
                .load(std::memory_order_relaxed));
        if (g_touchedCount.load(std::memory_order_relaxed) == 0) return orig(a_this, a_control);
        (isFind ? g_s5FindBrackets : g_s5Brackets).fetch_add(1, std::memory_order_relaxed);
        ViewBracket b;
        return orig(a_this, a_control);
    }

    // ======================================================================
    // Game thread.
    // ======================================================================

    // End the WINNING claim this entry belongs to: Harbinger releases it (the ch.20 model).
    // Caller holds NO lock.
    void End(RE::FormID a_id, std::uint32_t a_state, const char* a_why) {
        APMF_API::APMF_Param p{};
        float                basis = 0.0f;
        APMF_API::Handle     h     = APMF_API::kInvalidHandle;
        const bool winner = apmf::ControlMap::Get().TryGetOwningClaimBasis(a_id, APMF_API::kIntent_CombatApproach, p,
                                                                          basis, &h);
        std::uint32_t applied = 0, outside = 0;
        RE::FormID    tgt = 0;
        float         r = 0.0f, d = -1.0f;
        {
            std::unique_lock lk(g_entryMx);
            const auto it = g_entries.find(a_id);
            if (it == g_entries.end() || it->second->ending || it->second->released) return;
            Entry& e = *it->second;
            if (winner && h != e.owner && e.owner != APMF_API::kInvalidHandle) return;   // the claim moved
            SetLive(e, false);
            e.ending = true;
            SetState(e, a_state, apmf::clock::MonotonicMs());
            applied = e.applied.load(std::memory_order_relaxed);
            outside = e.outsideCount.load(std::memory_order_relaxed);
            tgt = e.target; r = e.radius; d = e.distance;
        }
        g_ended.fetch_add(1, std::memory_order_relaxed);
        MarkReactivate(a_id);   // F3: S3 puts the engine's area back on the actor's own combat thread
        const std::string line = fmt::format(
            "[ch.24] {} approach ENDED for 0x{} -> X 0x{} (R {:.0f}, last distance {:.0f}): the area carried the bound "
            "{} time(s), the engine judged the actor outside it {} time(s); claim h={} released by Harbinger.",
            a_why, Hex(a_id), Hex(tgt), r, d, applied, outside, winner ? h : 0u);
        // Principle 7: an engine-side failure is a warning; an ordinary end is info.
        if (a_state == APMF_API::kApproachState_EngineDropped) spdlog::warn("{}", line);
        else spdlog::info("{}", line);
        if (winner && h != APMF_API::kInvalidHandle) apmf::ControlMap::Get().EnqueueRelease(h);
    }

    void Apply(RE::FormID a_id, const APMF_API::APMF_Param& a_param, const char* a_what) {
        const std::uint64_t now = apmf::clock::MonotonicMs();
        {
            std::unique_lock lk(g_entryMx);
            auto& slot = g_entries[a_id];
            if (!slot) slot = std::make_unique<Entry>();
            Entry& e = *slot;
            SetLive(e, false);
            e.target     = a_param.target;
            e.radius     = a_param.fval;
            e.flags      = static_cast<std::uint32_t>(a_param.ival);
            e.owner      = APMF_API::kInvalidHandle;
            e.released   = false;
            e.ending     = false;
            e.wasApplied = false;
            e.distance   = -1.0f;
            e.claimMs    = now;
            e.inCombatSinceMs = 0;
            e.lastLineMs = 0;
            e.applied.store(0, std::memory_order_relaxed);
            e.lastSeenMs.store(0, std::memory_order_relaxed);
            e.lastApplyMs.store(0, std::memory_order_relaxed);
            e.seatFlags.store(0, std::memory_order_relaxed);
            e.inside.store(0, std::memory_order_relaxed);
            e.insideMs.store(0, std::memory_order_relaxed);
            e.outsideCount.store(0, std::memory_order_relaxed);
            e.state = APMF_API::kApproachState_None;   // force a state change (new seq) below
            SetState(e, APMF_API::kApproachState_Waiting, now);
        }
        spdlog::info("[ch.24] 0x{} combat-approach claim {}: close to within {:.0f} of 0x{}{}. The actor's own combat "
                     "area carries the bound while it fights; nothing else is claimed.",
                     Hex(a_id), a_what, a_param.fval, Hex(a_param.target),
                     (static_cast<std::uint32_t>(a_param.ival) & APMF_API::kApproach_Hold) != 0 ? " (HOLD after arrival)"
                                                                                                : " (ends on arrival)");
    }

    class CombatApproachChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "combat-approach"; }
        int              ChannelNo() const override { return 24; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_CombatApproach; }

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            Apply(id, param, "ENGAGED");
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            Apply(id, param, "RE-POINTED");
        }

        // Relinquish (INVARIANTS #5a): the seats stop carrying the bound; the next area update
        // (<= 0.25 s) puts the engine's own geometry back first and recomputes from it.
        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            std::uint32_t applied = 0;
            bool          had = false, ended = false;
            {
                std::unique_lock lk(g_entryMx);
                if (const auto it = g_entries.find(id); it != g_entries.end() && !it->second->released) {
                    had   = true;
                    Entry& e = *it->second;
                    SetLive(e, false);
                    ended = e.ending;
                    if (!e.ending) SetState(e, APMF_API::kApproachState_Released, apmf::clock::MonotonicMs());
                    e.released = true;
                    applied    = e.applied.load(std::memory_order_relaxed);
                }
            }
            if (had) MarkReactivate(id);   // F3
            if (had)
                spdlog::info("[ch.24] 0x{} combat-approach released ({}); the area carried the bound {} time(s). "
                             "The engine's own combat area answers again from its next update.",
                             Hex(id), ended ? "ended by Harbinger" : "by the client, an outranking claim or an unload",
                             applied);
        }
    };

    // Harbinger 1b: a HIGHER-PRIORITY combat area is current (UpdateAreas' own priority pick; the only
    // one above the standard area's 1 is a package's HoldPosition area, 3), so the bound yields. No
    // enforcement exists here (a package's force wins, the file header's DENY-COMPLETENESS), so this is
    // attribution only: named after the actor's RUNNING package -- the package whose HoldPosition
    // procedure adds that area -- through its plugin, by core/PackageSources.cpp's rule (GetFile(0); a
    // package created at runtime = Engine). Once per transition into YIELDED. GAME THREAD (Poll).
    void AttributeYield(RE::FormID a_id, RE::Actor* a_actor, APMF_API::Handle a_h) {
        const RE::TESPackage*   pkg = a_actor ? a_actor->GetCurrentPackage() : nullptr;
        apmf::sources::SourceId src = apmf::sources::kNoSource;
        if (pkg) {
            if (const auto* f = pkg->GetFile(0)) src = apmf::sources::Register(f->GetFilename(), apmf::sources::Kind::kPlugin);
            else src = apmf::sources::kEngineSource;
        }
        apmf::facetattr::Observe(apmf::sources::Facet::kCombat, src, a_id,
                                 "ch.24: a higher-priority combat area is current and the actor runs no package");
        spdlog::info("[ch.24] 0x{} the bound YIELDS to a higher-priority combat area (a package hold-position area): "
                     "named {} (running package 0x{}); it stands -- a package's force wins (claim h={}).",
                     Hex(a_id), apmf::facetattr::NameOr(src, "Unattributed"), Hex(pkg ? pkg->GetFormID() : 0), a_h);
    }

}

namespace apmf::combatapproach {

    void Install() {
        if (g_installTried.exchange(true)) return;

        const char* why = nullptr;
        if (REL::Module::IsVR()) {
            why = "VR runtime (the seats are verified on 1.6.1170, 1.5.97 and 1.7.104 only)";
        } else if (!apmf::allowance::RuntimeSupported()) {
            why = "runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104 (the seats are verified on those three only)";
        } else if (GetPrivateProfileIntA("CombatApproach", "bCombatApproach", 1, kIni) == 0) {
            why = "[CombatApproach] bCombatApproach=0 in Data/SKSE/Plugins/APMF.ini";
        }
        if (why) {
            g_notInstalledReason.store(why, std::memory_order_release);
            spdlog::warn("[ch.24] combat-approach seats NOT installed -- {}. kIntent_CombatApproach claims are REFUSED.",
                         why);
            return;
        }

        // VERIFY EVERYTHING BEFORE WRITING ANYTHING: a bound without its deny brackets would leak
        // into the target and cast facets, so the seat set is installed whole or not at all.
        REL::Relocation<std::uintptr_t> vtStd{ RE::VTABLE_CombatAreaStandard[0] };
        REL::Relocation<std::uintptr_t> vtSel{ RE::VTABLE_CombatTargetSelectorStandard[0] };
        bool ok = apmf::allowance::SeatVerified(vtStd.address(), "CombatApproach.CombatAreaStandard (0x05/0x06/0x0B)");
        ok = apmf::allowance::SeatVerified(vtSel.address(), "CombatApproach.CombatTargetSelectorStandard (0x06 bracket)") &&
             ok;

        const REL::VariantID kItemVtables[] = {
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterInvisibility_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterInvisibility_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemScroll_CombatMagicCasterInvisibility_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterInvisibility_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemShout_CombatMagicCasterInvisibility_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterBoundItem_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemStaff_CombatMagicCasterBoundItem_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemScroll_CombatMagicCasterBoundItem_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterBoundItem_[0],
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemShout_CombatMagicCasterBoundItem_[0],
        };
        auto leafId = [](std::string_view a_name) -> const REL::VariantID* {
            for (const auto& l : apmf::cbt::kLeaves)
                if (a_name == l.name) return &l.vtbl;
            return nullptr;
        };
        const REL::VariantID* idAttack = leafId("CombatBehaviorAttack");
        const REL::VariantID* idBash   = leafId("CombatBehaviorBash");
        const REL::VariantID* idFind   = leafId("CombatBehaviorFindWeapon");
        if (!idAttack || !idBash || !idFind) {
            spdlog::error("[ch.24] a leaf is missing from apmf::cbt::kLeaves (Attack / Bash / FindWeapon).");
            ok = false;
        } else {
            REL::Relocation<std::uintptr_t> a{ *idAttack }, b{ *idBash };
            ok = apmf::allowance::SeatVerified(a.address(), "CombatApproach.Leaf.CombatBehaviorAttack (0x02 bracket)") && ok;
            ok = apmf::allowance::SeatVerified(b.address(), "CombatApproach.Leaf.CombatBehaviorBash (0x02 bracket)") && ok;
            REL::Relocation<std::uintptr_t> fw{ *idFind };
            ok = apmf::allowance::SeatVerified(fw.address(), "CombatApproach.Leaf.CombatBehaviorFindWeapon (0x02 bracket)") && ok;
        }
        REL::Relocation<void*> expectedTD{ RE::RTTI_CombatInventoryItem };
        ok = apmf::allowance::SeatVerified(reinterpret_cast<std::uintptr_t>(expectedTD.get()),
                                           "CombatApproach.RTTI.CombatInventoryItem") && ok;
        for (const auto& id : kItemVtables) {
            REL::Relocation<std::uintptr_t> vt{ id };
            if (!apmf::allowance::SeatVerified(vt.address(), "CombatApproach.CombatInventoryItemMagicT (0x0F bracket)")) {
                ok = false;
                continue;
            }
            if (!apmf::allowance::DerivesFrom(vt.address(), expectedTD.get())) {
                spdlog::error("[ch.24] vtable RVA 0x{} does NOT derive CombatInventoryItem.",
                              Hex(vt.address() - REL::Module::get().base(), 0));
                ok = false;
            }
        }
        if (!ok) {
            g_notInstalledReason.store("the address self-check refused a seat vtable (listed above)",
                                       std::memory_order_release);
            spdlog::error("[ch.24] combat-approach seats NOT installed (self-check refused a vtable, listed above). "
                          "kIntent_CombatApproach claims are REFUSED.");
            return;
        }

        g_vtStd = vtStd.address();
        g_vtSel = vtSel.address();
        for (const auto& id : kItemVtables) {
            REL::Relocation<std::uintptr_t> vt{ id };
            g_origShouldEquip[vt.address()] = vt.write_vfunc(kSlotShouldEquip, &ShouldEquipThunk);
            apmf::sources::NoteVfunc(apmf::sources::Facet::kCombat,
                                     fmt::format("CombatApproach.Item@vt0x{:X}.ShouldEquip", vt.address() - REL::Module::get().base()),
                                     vt.address(), kSlotShouldEquip, SKSE::stl::unrestricted_cast<std::uintptr_t>(&ShouldEquipThunk),
                                     g_origShouldEquip[vt.address()]);
        }
        g_origSelect.store(vtSel.write_vfunc(kSlotSelect, &SelectThunk), std::memory_order_relaxed);
        {   // S5 (all deny brackets go in before S1 can carry a bound)
            REL::Relocation<std::uintptr_t> a{ *idAttack }, b{ *idBash };
            g_vtAttack.store(a.address(), std::memory_order_relaxed);
            g_vtBash.store(b.address(), std::memory_order_relaxed);
            g_origAttackAct.store(a.write_vfunc(0x02, &LeafActThunk), std::memory_order_relaxed);
            g_origBashAct.store(b.write_vfunc(0x02, &LeafActThunk), std::memory_order_relaxed);
            REL::Relocation<std::uintptr_t> fw{ *idFind };   // APMF-B57: the Acquire Weapon link is exempt from the bound
            g_vtFind.store(fw.address(), std::memory_order_relaxed);
            g_origFindAct.store(fw.write_vfunc(0x02, &LeafActThunk), std::memory_order_relaxed);
        }
        g_origIsInsidePos.store(vtStd.write_vfunc(kSlotIsInsidePos, &IsInsidePosThunk), std::memory_order_relaxed);
        g_origIsInsideLoc.store(vtStd.write_vfunc(kSlotIsInsideLoc, &IsInsideLocThunk), std::memory_order_relaxed);
        g_origUpdate.store(vtStd.write_vfunc(kSlotUpdate, &UpdateThunk), std::memory_order_relaxed);
        {   // Harbinger 1a seat census: the six single seats above (census rows only)
            using apmf::sources::Facet;
            using apmf::sources::NoteVfunc;
            NoteVfunc(Facet::kCombat, "CombatApproach.Selector.Select", vtSel.address(), kSlotSelect,
                      SKSE::stl::unrestricted_cast<std::uintptr_t>(&SelectThunk), g_origSelect.load());
            NoteVfunc(Facet::kCombat, "CombatApproach.AttackLeaf.Act", g_vtAttack.load(), 0x02,
                      SKSE::stl::unrestricted_cast<std::uintptr_t>(&LeafActThunk), g_origAttackAct.load());
            NoteVfunc(Facet::kCombat, "CombatApproach.BashLeaf.Act", g_vtBash.load(), 0x02,
                      SKSE::stl::unrestricted_cast<std::uintptr_t>(&LeafActThunk), g_origBashAct.load());
            NoteVfunc(Facet::kCombat, "CombatApproach.AreaStandard.IsInsidePos", vtStd.address(), kSlotIsInsidePos,
                      SKSE::stl::unrestricted_cast<std::uintptr_t>(&IsInsidePosThunk), g_origIsInsidePos.load());
            NoteVfunc(Facet::kCombat, "CombatApproach.AreaStandard.IsInsideLoc", vtStd.address(), kSlotIsInsideLoc,
                      SKSE::stl::unrestricted_cast<std::uintptr_t>(&IsInsideLocThunk), g_origIsInsideLoc.load());
            NoteVfunc(Facet::kCombat, "CombatApproach.AreaStandard.Update", vtStd.address(), kSlotUpdate,
                      SKSE::stl::unrestricted_cast<std::uintptr_t>(&UpdateThunk), g_origUpdate.load());
            // Harbinger 1b: a module hooked inside S1 (installed before Harbinger) produces the area S1
            // substitutes; with none, the engine does.
            if (const auto prev = apmf::sources::PrevSourceOf("CombatApproach.AreaStandard.Update");
                prev != apmf::sources::kNoSource)
                g_s1Src.store(prev, std::memory_order_relaxed);
        }
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[ch.24] combat-approach seats installed: CombatAreaStandard Update (0x0B, carries the bound), "
                     "IsInside(WorldLocation) (0x05, the engine's own area inside a deny bracket), IsInside(point) "
                     "(0x06, observe-only); deny brackets on CombatTargetSelectorStandard SelectTarget (0x06) and "
                     "{} invisibility / bound-item CheckShouldEquip (0x0F) and Attack / Bash / FindWeapon act (0x02). All "
                     "chaining.",
                     g_origShouldEquip.size());
    }

    bool Installed() { return g_installed.load(std::memory_order_acquire); }

    const char* NotInstalledReason() {
        const char* r = g_notInstalledReason.load(std::memory_order_acquire);
        return r ? r : "unknown";
    }

    void Poll() {
        if (!g_installed.load(std::memory_order_relaxed)) return;
        const std::uint64_t now = apmf::clock::MonotonicMs();
        if (g_lastTickMs != 0)
            g_worldMs.fetch_add((std::min)(now - g_lastTickMs, kTickClampMs), std::memory_order_relaxed);
        g_lastTickMs = now;
        const std::uint64_t wnow = WorldNow();
        static std::uint64_t s_lastMs = 0;
        if (now - s_lastMs < kPollMs) return;
        s_lastMs = now;

        // A record S1 has not rewritten for kPurgeMs of WORLD time belongs to an area the engine no
        // longer updates (its fight ended and the controller is gone): forget it. A record waiting for
        // its F3 reactivation is kept far longer (kPurgeDeadMs), so S3 or RestoreBeforeSave can put the
        // engine's area back first. Keys are
        // never dereferenced here -- only S1 (with `this` == the key) and RestoreBeforeSave (after a
        // live-controller membership test) ever touch the area itself.
        if (g_touchedCount.load(std::memory_order_relaxed) != 0) {
            static std::uint64_t s_lastPurgeMs = 0;
            if (wnow - s_lastPurgeMs >= 1000) {
                s_lastPurgeMs = wnow;
                std::unique_lock lk(g_areaMx);
                for (auto it = g_touched.begin(); it != g_touched.end();) {
                    if (wnow - it->second.ms > (it->second.reactivate ? kPurgeDeadMs : kPurgeMs)) {
                        if (it->second.reactivate) g_reactivateCount.fetch_sub(1, std::memory_order_relaxed);
                        it = g_touched.erase(it);
                    } else {
                        ++it;
                    }
                }
                g_touchedCount.store(g_touched.size(), std::memory_order_relaxed);
            }
        }

        std::vector<RE::FormID> ids;
        {
            std::shared_lock lk(g_entryMx);
            for (const auto& [id, e] : g_entries)
                if (!e->released && !e->ending) ids.push_back(id);
        }

        for (const RE::FormID id : ids) {
            APMF_API::APMF_Param p{};
            float                basis = 0.0f;
            APMF_API::Handle     h     = APMF_API::kInvalidHandle;
            if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(id, APMF_API::kIntent_CombatApproach, p, basis, &h))
                continue;   // not published yet, or released: Engage / Release will say

            RE::FormID    target = 0;
            float         radius = 0.0f;
            std::uint32_t flags  = 0;
            bool          wasApplied = false;
            {
                std::unique_lock lk(g_entryMx);
                const auto it = g_entries.find(id);
                if (it == g_entries.end() || it->second->released || it->second->ending) continue;
                Entry& e = *it->second;
                if (e.target != p.target) continue;   // mid re-point: the next Apply rewrites the entry
                e.owner    = h;
                target     = e.target;
                radius     = e.radius;
                flags      = e.flags;
                wasApplied = e.wasApplied;
            }

            if (target == 0 || target == id || !std::isfinite(radius) || !(radius > 0.0f)) {
                End(id, APMF_API::kApproachState_Refused,
                    "invalid declaration (a Repoint named target 0 / the actor itself, or a radius that is not a "
                    "positive finite number):");
                continue;
            }
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!actor || actor->IsDead() || !actor->Is3DLoaded()) {
                End(id, APMF_API::kApproachState_ActorGone, "actor dead / unloaded:");
                continue;
            }
            auto*       x   = RE::TESForm::LookupByID<RE::TESObjectREFR>(target);
            const char* why = nullptr;
            if (!x) why = "X is not a loaded reference:";
            else if (x->IsDeleted() || x->IsDisabled()) why = "X deleted / disabled:";
            else if (!x->Is3DLoaded()) why = "X unloaded:";
            else if (auto* xa = x->As<RE::Actor>(); xa && xa->IsDead()) why = "X dead:";
            const void* spaceA = SpaceOf(actor);
            const void* spaceX = x ? SpaceOf(x) : nullptr;
            if (!why && (!spaceX || spaceX != spaceA)) why = "X in another cell space:";
            if (why) {
                End(id, APMF_API::kApproachState_TargetLost, why);
                continue;
            }

            const RE::NiPoint3 posXReal = x->GetPosition();
            RE::NiPoint3       posX     = posXReal;   // the goal point; clamped just inside a grazing leash below
            float              dist     = actor->GetPosition().GetDistance(posX);
            // A combat SUBSTRATE (S2a) is not a fight: a waiting ch.24 claim does not apply under it.
            const bool         inCombat = actor->IsInCombat() && !apmf::combatsubstrate::Suppressing(id);

            // A ch.23 leash on the same actor: some point within R of X must lie inside it.
            bool leashed = false;
            bool clamped = false;
            {
                APMF_API::APMF_Param lp{};
                if (apmf::ControlMap::Get().TryGetOwningClaim(id, APMF_API::kIntent_PursuitLeash, lp) && lp.target != 0) {
                    auto* anchor = RE::TESForm::LookupByID<RE::Actor>(lp.target);
                    if (anchor && anchor->Is3DLoaded() && SpaceOf(anchor) == spaceX) {
                        const RE::NiPoint3 ap = anchor->GetPosition();
                        const float        dx = posXReal.GetDistance(ap);
                        if (dx - radius > lp.fval) {
                            leashed = true;
                        } else if (dx > lp.fval && dx > 0.0f) {
                            // APMF-B55 (marth 2026-10-11): the disc only GRAZES the leash (X itself is outside it).
                            // The goal point is clamped to just inside the leash disc, along the anchor -> X line,
                            // so the engine's path never ends at a point outside the leash.
                            const float k = std::max(0.0f, lp.fval - kLeashClampMargin) / dx;
                            posX    = RE::NiPoint3{ ap.x + (posXReal.x - ap.x) * k, ap.y + (posXReal.y - ap.y) * k,
                                                    ap.z + (posXReal.z - ap.z) * k };
                            dist    = actor->GetPosition().GetDistance(posX);
                            clamped = true;
                        }
                    }
                }
            }

            std::uint32_t next     = APMF_API::kApproachState_Waiting;
            bool          live     = false;
            const char*   endWhy   = nullptr;
            std::uint32_t endState = 0;
            bool          logLine  = false;
            std::uint32_t applied = 0, sf = 0, inside = 0, outside = 0;
            std::uint32_t prevState = 0;
            {
                std::unique_lock lk(g_entryMx);
                const auto it = g_entries.find(id);
                if (it == g_entries.end() || it->second->released || it->second->ending) continue;
                Entry& e = *it->second;
                e.distance = dist;
                applied    = e.applied.load(std::memory_order_relaxed);
                if (applied != 0) e.wasApplied = wasApplied = true;
                sf      = e.seatFlags.load(std::memory_order_relaxed);
                outside = e.outsideCount.load(std::memory_order_relaxed);
                const std::uint64_t insMs = e.insideMs.load(std::memory_order_relaxed);
                inside = (insMs != 0 && wnow - insMs <= kVerdictMs) ? e.inside.load(std::memory_order_relaxed) : 0;

                if (!inCombat) {
                    e.inCombatSinceMs = 0;
                    if (wasApplied) { endWhy = "combat over:"; endState = APMF_API::kApproachState_CombatEnded; }
                } else if (leashed) {
                    next = APMF_API::kApproachState_Leashed;
                } else {
                    if (!e.live) {   // (re)starting to carry the bound: the stall clock starts now
                        e.inCombatSinceMs = wnow;
                        e.lastSeenMs.store(0, std::memory_order_relaxed);
                    }
                    live = true;
                    if (sf & (kSeatYielded | kSeatStood)) next = APMF_API::kApproachState_Yielded;
                    else if (sf & kSeatDisabled) next = APMF_API::kApproachState_Disabled;
                    else if (inside == 2 || (inside == 0 && dist <= radius)) {
                        if ((flags & APMF_API::kApproach_Hold) == 0 && applied != 0) {
                            endWhy = "arrived:"; endState = APMF_API::kApproachState_Arrived;
                        } else {
                            next = APMF_API::kApproachState_Holding;
                        }
                    } else {
                        next = APMF_API::kApproachState_Approaching;
                    }
                    // Principle 7: a bound the engine stopped carrying is a FAILURE, said out loud.
                    const std::uint64_t seen  = e.lastSeenMs.load(std::memory_order_relaxed);
                    const std::uint64_t since = seen != 0 ? seen : e.inCombatSinceMs;
                    if (!endWhy && wnow - since > kStallMs) {
                        endWhy   = seen != 0 ? "the engine stopped updating the bound for 3 s (its own path to X failed "
                                               "and it deactivated the area, or its combat update stopped):"
                                             : "the engine never updated the actor's standard combat area in 3 s of "
                                               "combat (no area, a paused combat update, or another mod replaced the "
                                               "seat):";
                        endState = APMF_API::kApproachState_EngineDropped;
                    }
                }
                if (!endWhy) {
                    e.want = Geo{ posX.x, posX.y, posX.z, spaceX, radius };
                    SetLive(e, live);
                    prevState = e.state;
                    SetState(e, next, now);
                    if (now - e.lastLineMs >= kLineMs) {
                        e.lastLineMs = now;
                        logLine      = true;
                    }
                }
            }
            if (endWhy) {
                End(id, endState, endWhy);
                continue;
            }
            if (next == APMF_API::kApproachState_Yielded && prevState != next && (sf & kSeatYielded) != 0)
                AttributeYield(id, actor, h);
            if (logLine)
                spdlog::info("[ch.24] 0x{} approach {} -> X 0x{}: distance {:.0f} / R {:.0f}; area carried the bound {} "
                             "time(s); engine: {}, standard area {}; outside answers {}{}.",
                             Hex(id), StateName(next), Hex(target), dist, radius, applied,
                             inside == 2 ? "INSIDE" : inside == 1 ? "OUTSIDE (return-to-area selectable)" : "no fresh verdict",
                             (sf & kSeatCurrent) ? "CURRENT" : (sf & kSeatYielded) ? "outranked"
                                                     : (sf & kSeatStood) ? "kept (the claim does not outrank its source)"
                                                                         : "not current",
                             outside, clamped ? "; goal point CLAMPED just inside the ch.23 leash (X grazes it)" : "");
        }
    }

    void Heartbeat() {
        if (!g_installed.load(std::memory_order_relaxed)) return;
        std::size_t n = 0;
        {
            std::shared_lock lk(g_entryMx);
            for (const auto& [id, e] : g_entries)
                if (!e->released) ++n;
        }
        const std::size_t ni = g_intCount.load(std::memory_order_relaxed);
        if (n == 0 && ni == 0) return;
        const std::uint64_t now = apmf::clock::MonotonicMs();
        if (now - g_lastHeartbeatMs < kHeartbeatMs) return;
        g_lastHeartbeatMs = now;
        spdlog::info("[ch.24] heartbeat: {} claim(s), {} Harbinger-internal bound(s) (held engagement / leash return; "
                     "written {}); standard-area updates seen {} (any actor), bound applied {}, "
                     "engine geometry restored first {}, outranked {}, disabled {}, not written by priority {}, "
                     "rewritten outside the seat (drift) {}; vanilla answers inside deny "
                     "brackets {} (target-selector brackets {}, inv/bound brackets {}, attack/bash brackets {}, find-weapon brackets {}); "
                     "engine inside/outside verdicts on the bound {} ({} outside); ended by Harbinger {}; engine areas "
                     "put back after an end {}; areas carrying a bound now {}.",
                     n, ni, g_s1Internal.load(), g_s1Seen.load(), g_s1Applied.load(), g_s1Restored.load(), g_s1Yielded.load(),
                     g_s1Disabled.load(), g_s1Stood.load(), g_s1Drift.load(), g_s2Vanilla.load(), g_s3Brackets.load(), g_s4Brackets.load(),
                     g_s5Brackets.load(), g_s5FindBrackets.load(), g_s6Verdicts.load(), g_s6Outside.load(), g_ended.load(), g_s3PutBack.load(),
                     g_touchedCount.load());
    }

    void RestoreBeforeSave() {
        if (g_touchedCount.load(std::memory_order_relaxed) == 0) return;
        // F6 (review on 274d7e4): SKSE sends kSaveGame from the save path of Main::Update, the thread
        // the engine's own change-form save (CombatController::SaveGame) runs on right after. Say so
        // loudly if that ever stops being the thread the player's update seat runs on.
        if (!apmf::hook::OnMainThread()) {
            static std::atomic<bool> s_warned{ false };
            if (!s_warned.exchange(true))
                spdlog::warn("[ch.24] save: kSaveGame arrived OFF the main (player update) thread -- the area "
                             "restore below races the combat jobs. (Logged once.)");
        }
        std::size_t restored = 0, dropped = 0;
        std::unique_lock lk(g_areaMx);
        for (auto it = g_touched.begin(); it != g_touched.end(); it = g_touched.erase(it)) {
            const Touched& t = it->second;
            // Only an area that is provably still alive: the actor's CURRENT controller is the one
            // S1 saw, and the area is in that controller's own areas array.
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(t.actor);
            auto* cc    = actor ? actor->GetActorRuntimeData().combatController : nullptr;
            bool  live  = false;
            if (cc && static_cast<void*>(cc) == t.ctl) {
                for (auto* a : cc->GetRuntimeData().areas)
                    if (static_cast<void*>(a) == it->first) { live = true; break; }
            }
            // PutBack also re-activates the area of a FINISHED claim (F3), so neither X / R nor the
            // engine's failed-path deactivation reaches the .ess. A LIVE claim's area found INACTIVE
            // (+0x2C == 0) carrying our bound was deactivated by the engine's own failed path to X --
            // nothing else clears it on an area S1 wrote -- so it is re-activated too (review R2-2).
            Touched tt = t;
            if (live && At<std::uint8_t>(it->first, kAreaActive) == 0) tt.reactivate = true;
            if (live && PutBack(it->first, tt)) ++restored;
            else ++dropped;
        }
        g_touchedCount.store(0, std::memory_order_relaxed);
        g_reactivateCount.store(0, std::memory_order_relaxed);
        lk.unlock();
        spdlog::info("[ch.24] save: put the engine's own geometry back into {} combat area(s) carrying a bound ({} "
                     "no longer alive or already overwritten). The bound is re-applied at the next area update.",
                     restored, dropped);
    }

    void ResetAll(const char* why) {
        std::size_t n = 0, a = 0;
        {
            std::unique_lock lk(g_entryMx);
            n = g_entries.size();
            g_entries.clear();
            g_liveCount.store(0, std::memory_order_relaxed);
        }
        {
            std::unique_lock lk(g_intMx);
            n += g_internal.size();
            g_internal.clear();
            g_intCount.store(0, std::memory_order_relaxed);
        }
        {
            std::unique_lock lk(g_areaMx);
            a = g_touched.size();
            g_touched.clear();
            g_touchedCount.store(0, std::memory_order_relaxed);
            g_reactivateCount.store(0, std::memory_order_relaxed);
        }
        if (n != 0 || a != 0)
            spdlog::info("[ch.24] {} -- dropped {} combat-approach entr{} and {} area record(s).", why, n,
                         n == 1 ? "y" : "ies", a);
    }

    std::uint32_t GetState(RE::FormID actor, APMF_API::APMF_CombatApproachInfo* out) {
        std::uint32_t state = APMF_API::kApproachState_None;
        APMF_API::APMF_CombatApproachInfo info{};
        info.size     = sizeof(info);
        info.actor    = actor;
        info.distance = -1.0f;
        {
            std::shared_lock lk(g_entryMx);
            if (const auto it = g_entries.find(actor); it != g_entries.end()) {
                const Entry& e = *it->second;
                state            = e.state;
                info.target      = e.target;
                info.radius      = e.radius;
                info.distance    = e.distance;
                const std::uint64_t ms = apmf::clock::MonotonicMs() - e.stateSinceMs;
                info.msInState   = ms > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(ms);
                info.seq         = e.seq;
                info.ownerHandle = e.owner;
                info.flags       = e.flags;
                info.applied     = e.applied.load(std::memory_order_relaxed);
                info.engineInside = e.inside.load(std::memory_order_relaxed);
            }
        }
        info.state = state;
        if (out && out->size >= APMF_API::kCombatApproachInfoV19Size) {
            const std::uint32_t callerSize = out->size;
            std::memcpy(out, &info, APMF_API::kCombatApproachInfoV19Size);
            out->size = callerSize;
        }
        return state;
    }

    const void* SpaceOfRef(const RE::TESObjectREFR* a_ref) { return SpaceOf(a_ref); }

    bool SetInternalBound(RE::FormID a_actor, const RE::NiPoint3& a_x, const void* a_space, float a_radius,
                          APMF_API::Handle a_owner, const char* a_who) {
        if (!g_installed.load(std::memory_order_relaxed) || !a_space || !(a_radius > 0.0f)) return false;
        std::unique_lock lk(g_intMx);
        auto& slot = g_internal[a_actor];
        if (!slot) slot = std::make_unique<Internal>();
        slot->want.x      = a_x.x;
        slot->want.y      = a_x.y;
        slot->want.z      = a_x.z;
        slot->want.space  = a_space;
        slot->want.radius = a_radius;
        slot->owner       = a_owner;
        slot->who         = a_who ? a_who : "";
        g_intCount.store(g_internal.size(), std::memory_order_relaxed);
        return true;
    }

    void ClearInternalBound(RE::FormID a_actor) {
        bool had = false;
        {
            std::unique_lock lk(g_intMx);
            if (const auto it = g_internal.find(a_actor); it != g_internal.end()) {
                g_internal.erase(it);
                had = true;
            }
            g_intCount.store(g_internal.size(), std::memory_order_relaxed);
        }
        if (!had) return;
        // F3, as for a finished claim: S3 puts the engine's own area back (and re-activates it if the
        // engine's failed Return To Combat Area path cleared it). Not while a ch.24 claim is live on the
        // actor: S1 is carrying that claim's bound in the same area now.
        bool clientLive = false;
        {
            std::shared_lock lk(g_entryMx);
            const auto it = g_entries.find(a_actor);
            clientLive    = it != g_entries.end() && it->second->live;
        }
        if (!clientLive) MarkReactivate(a_actor);
    }

    bool GetInternalInfo(RE::FormID a_actor, InternalInfo& a_out) {
        a_out = InternalInfo{};
        {
            std::shared_lock lk(g_intMx);
            const auto it = g_internal.find(a_actor);
            if (it == g_internal.end()) return false;
            a_out.applied = it->second->applied.load(std::memory_order_relaxed);
            a_out.stood   = it->second->stood.load(std::memory_order_relaxed);
            const auto last  = it->second->lastApplyMs.load(std::memory_order_relaxed);
            const auto wnow  = WorldNow();
            a_out.msSinceApply = last == 0 ? ~0ull : wnow - (std::min)(last, wnow);
        }
        std::shared_lock elk(g_entryMx);
        const auto eit = g_entries.find(a_actor);
        a_out.clientWins = eit != g_entries.end() && eit->second->live;
        return true;
    }

}

APMF_REGISTER_CHANNEL(CombatApproachChannel);
