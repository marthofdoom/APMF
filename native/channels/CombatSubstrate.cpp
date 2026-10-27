#include "PCH.h"
#include "channels/CombatSubstrate.h"

#include "channels/AnchorGuard.h"          // the null-target join refusal's group set + the anchor-leak sweep
#include "channels/CombatApproach.h"       // SpaceOfRef: the anchor's last-known location space
#include "channels/CombatEntry.h"          // hold::StopCombatRanked: the empty-fight end (F6)
#include "channels/CombatReentryDeny.h"    // the slot-0x99 seat this file's anchor refusal rides; ClientEntryScope
#include "channels/SubstrateAggressor.h"   // R5 F-3: the aggressor-bit clear (one-line call-outs only)
#include "channels/SubstrateBark.h"        // R5 F-2: the combat-start bark deny (one-line call-outs only)
#include "channels/SubstrateEquip.h"       // the spell-equip path the allow-list opens (one-line call-outs only)
#include "channels/SubstrateProbe.h"       // R1 passive probes (one-line call-outs only)
#include "channels/SubstratePackage.h"     // F-6 override deny + S2b re-assert (one-line call-outs only)
#include "core/ActionGate.h"               // SubstrateLeavesPaired: the allow-list's coverage
#include "core/Allowance.h"                // SeatVerified, RuntimeSupported
#include "core/CasterTypeCensus.h"         // NotifyInstalled: the fire signal
#include "core/Clock.h"
#include "core/BehaviorTreeCatalog.h"         // the leaf catalog (names)
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/PackageGate.h"              // EvaluatePackage's verified row is shared
#include "core/Registry.h"
#include "core/Sources.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ============================================================================
// THE COMBAT SUBSTRATE (kIntent_CombatSubstrate, ABI v23) -- slice S2a.
// Design: MFO Docs/COMBAT-SUBSTRATE-DESIGN.md (sections 3.1-3.7, 5), INVARIANTS #0 (j).
//
// marth 2026-10-12, verbatim: "Getting animations for everything is non negotiable." Every
// animated cast Harbinger knows of is the follower's OWN AI casting through the ch.8b seats,
// and those seats only exist inside a CombatController (CombatInventory / the casters are
// built in CombatController::Update only). Out of combat there is no controller, so an
// out-of-combat cast has no animated road. The substrate forces combat as a SUBSTRATE
// (CLAUDE.md principle 1): a controller whose only target is a Harbinger-owned ANCHOR actor,
// the cast leaves let through, every other facet the controller switches on DENIED.
//
// THE ANCHOR (design 3.3; marth: "If it can't be the player, 1 is fine. Use a clone of nazeem.")
//   ONE persistent actor in APMF.esl: NPC_ 0x808 (Nazeem 00013BBF authored field by field: no
//   factions, packages, outfit, inventory or script; not Unique; essential + ghost on the base;
//   aggression 0, assistance 0), ACHR 0x80A in the interior holding cell 0x809, home XMarker
//   REFR 0x80B (APMF_GenerateESL.py). It must be an ACTOR: StartCombat reads target+0x204 (AE)
//   and target+0xF8 before it builds anything (PROVEN, AE 0x6B6AED / 0x6B6B4F), past the end of
//   a 0x98-byte TESObjectREFR. The follower itself is refused at AE 0x6B69B3 / SE 0x625231; no
//   target builds no controller (AE 0x6B6CBE). At runtime Harbinger turns its AI off
//   (Actor::boolBits kProcessMe, bit 1 -- the same bit word StartCombat tests for kDead, bit 11,
//   at AE +0xE8 / SE +0xE0), keeps it 72 units BEHIND the holder (its collision is NOT switched off:
//   the only setter writes a form flag CommonLib marks unverified), makes it invisible (Character slot 0xE1 SetAlpha(0): AE
//   0x6C1A50 / SE 0x62FAD0; it applies only to a high-process actor, so it is re-applied on every
//   move) and moves it with the engine's own MoveTo (TESObjectREFR::MoveTo_Impl AE 56626 /
//   SE 56227, verified row Substrate.TESObjectREFR.MoveTo; home = MoveTo(the XMarker)). It is
//   REFERENCE-COUNTED: the first holder brings it out, the last close sends it home. It stands
//   at the holder whose claimed spell has the SHORTEST delivery range (self < touch < aimed <
//   no claim; a tie goes to the earliest open) and every other holder farther than its own
//   spell's reach from it logs one "waits" line (its claim is not cancelled).
//
// OPEN (game thread, design 3.6 "Open"):
//   1. validate: loaded, alive, enabled, AI process, HIGH process, NOT in combat (no
//      controller), in no scene, no dialogue menu open; a free slot; the substrate's client
//      (or, for the dry run, the client default) outranks Engine on CombatAction, Combat, Equip
//      and Detection (APMF_Sources.ini) -- otherwise a deny would not stand and the substrate
//      would switch facets on without denying them, so it does not open (principle 2);
//   2. the holder enters the lock-free slot table (the allow-list, the anchor refusal and the
//      knowledge hold read it from here on); the anchor comes out on 0 -> 1;
//   3. the FLOORS: two internal kIntent_Cast deny-only claims (right, left) at the lowest
//      basis, so any client cast claim on a hand outranks them there and the floor holds the
//      OTHER hand -- the hand block (core/HandBlock.cpp, core/EquipSink.cpp), EquipGate 0x0F and
//      CastGate 0x0A then keep every weapon, shield, torch and unclaimed spell out of the hands
//      (a full floor, NOT kCastFlag_FloorSpellsOnly). They rank as the substrate's own client
//      (sources::InheritClient). StartCombat's re-arm equip runs INSIDE the entry call, so the
//      entry waits until both floors are PUBLISHED (the next Drain);
//   4. ONE Actor::StartCombat(anchor, nullptr) -- the call ch.21 makes, verified row
//      CombatEntry.Actor.StartCombat, inside reentrydeny::ClientEntryScope (a declared entry
//      passes a ch.22 deny, INVARIANTS #0 (h) condition 4). A false return ends the substrate;
//   5. S2a (order corrected by review F2 of a512763; the design's 3.4 said set-then-evaluate):
//      Actor::EvaluatePackage(false, false) once (resetAI stays false) FIRST; once the engine has
//      run that re-pick (WriteBitAfterRepick: kEvpBuffered cleared, then one more Poll), the
//      process ignoring-combat bit (AIProcess+0x139, read by AE 0x6D6780 / SE 0x643F00) is set
//      ONCE. The controller is NOT paused (its +0x42 was copied from the bit, still 0, at birth:
//      AE 0x5582F8 / SE 0x4FCC6F). The package gate (AE 0x6D8950 via 0x6B6DF0) lets the follower's
//      own package run, so it keeps walking under it (marth: "block combat from affecting
//      movement and have them continue using the non combat package for movement"). The engine
//      re-syncs the bit from the package flag on a re-evaluation (UpdateIgnoringCombat AE
//      0x6D7E80); S2a does NOT re-assert it -- the P6 trace counts every reset. The per-frame
//      re-assert is S2b, built only after P6 passes in the field (marth's approval of record).
//
// THE DENY SET (design 3.5; deny-completeness, principle 2):
//   * every combat behaviour-tree LEAF is force-failed (ch.7's act()/pop() PAIR,
//     core/ActionGate.cpp) except CastImmediateSpell, CastConcentrationSpell and
//     PrepareDualCast -- and those three run only while the holder has a live DRIVING cast
//     claim (a substrate with nothing claimed casts nothing: the dry run). The SPELL-EQUIP PATH
//     runs under the same condition (feat/apmf-substrate-equip, channels/SubstrateEquip.cpp): the
//     EquipSpell leaf and the "Acquire Action Resource" instance of AcquireResource<2> (its
//     "Movement" instance stays denied); without them the Action branch, the only road to the
//     equip context, the spell equip and the Magic link, was cut for every holder. The catalog's
//     STRUCTURAL nodes (the Child / Fallback selectors, DynamicConditionalNode, Parallel,
//     Sequence, Repeat, ForceFail, ForceSuccess) are not leaves: they route to children and
//     decide nothing, and the magic subtree is built from them (AE builder 0x8A0720: Magic
//     Context > Casting Type Selector [ChildSelector<Value>] > Repeat > Cast Immediate Spell
//     Selector [FallbackSelector<NextChild>] > {PrepareDualCast, CastImmediateSpell, ...}), so
//     they pass, or the cast leaf could never be reached. Allow, not block: a leaf nobody
//     listed is denied by construction. The Movement half's leaves are leaves: denied, so the
//     package is the only driver of the body. COVERAGE (review F1 of a512763): EVERY
//     CombatBehaviorTreeNodeObject* vtable the MIT fork ships -- the 70 parameterless leaves,
//     the four search leaves (ch.23's act()/pop()) and the 21 PARAMETERISED nodes
//     (core/BehaviorTreeCatalog.h kParamLeaves: AcquireResource, Flee<float>, FaceAngle, WatchTarget,
//     DisableAim, FollowPath / AcquireItem<AcquireWeapon>, Land, the blackboard nodes, all denied
//     (12); the dual-cast CastImmediate / CastConcentration<bool>, gated like the cast leaves (2);
//     Repeat<...>, SpawnParallel, DynamicSelector<Conditional,float> and the two Idle waits,
//     structural, passing (7): a pure wait decides nothing, and denying it could leave a Repeat'd
//     subtree with no node that yields, round-2 review). The
//     substrate refuses to install unless all 95 carry the pair. Composite node classes that are
//     not NodeObject* (the CreateContext / ValueNode / conditional wrappers) decide nothing and
//     are not hooked;
//   * StartCombat against the ANCHOR by anyone but a live holder: the slot-0x99 seat ch.22
//     owns answers "dead" at StartCombat's TARGET check (verified row
//     Substrate.StartCombat.TargetIsDeadCall: AE +0xCC, return 0x6B6A0A; SE +0xCA, return
//     0x625288). StartCombat's own `this` is in rdi there; a 17-byte stub in front of the
//     thunk hands it in as a third argument (channels/CombatReentryDeny.cpp). The GROUP JOIN
//     (StartCombat's third argument) the design recorded as a hole is CLOSED since 2026-10-19
//     (SEV-1, a non-holder fought the anchor): at StartCombat's SELF check a NULL-target entry
//     into a substrate group by a non-holder is refused (channels/AnchorGuard.cpp), and a
//     route-independent sweep ERROR-logs and cleans any non-holder that targets the anchor or
//     whose group lists it, every Poll while a substrate is live and 10 s after (CLOSE runs it on
//     the closing holder's group members);
//   * the anchor's group entry is kept KNOWN (the held engagement's own un-lose write,
//     INVARIANTS #0 (i) condition 1, for the anchor only) so the group never searches for it;
//   * other spells: the floors + ch.8 / 0x0F / 0x0A as before; only the claimed spell runs;
//   * the combat-start BARK (R5 F-2, channels/SubstrateBark.cpp): the controller's first update (AE 33231 /
//     SE 32484) requests NormalToCombat / NormalToAlert; the holder group's two detection-dialogue timers are
//     pre-armed in the holder's own UpdateCombat before it runs, so the engine's own gate skips the line;
//   * the AGGRESSOR BIT (R5 F-3, channels/SubstrateAggressor.cpp): StartCombat marks the holder the aggressor
//     (AIProcess+0x136 bit 2); it is set false through the engine's setter right after the entry and again only
//     if something sets it while HELD.
//   StartCombat's block at AE 0x6B6CFB-0x6B6D32 (SE 0x625572-0x6255A9), once recorded here as the "combat-start
//   topic", says NO line (R2 research F-1): it tells the actor's SCENE that it entered combat (vfunc 0x4A into
//   24131; the substrate never opens on an actor in a scene) and clears head-track slot 5 (39472). KNOWN HOLE
//   approved by marth 2026-10-12, logged once per substrate: the combat state event (TESCombatEvent /
//   OnCombatStateChanged; R5 adds only the passive sink-order probe, F-4 is not built).
//
// REAL COMBAT (design 3.6a; marth: "If real combat starts, remove the anchor and drop
// suppression of combat behavior"). Three triggers, on whatever thread sees them: (a) a target
// other than the anchor in a holder's group (the detection seat, run synchronously from
// CombatGroup::AddTarget, and the game-thread poll of the group's targets); (b) a TESHitEvent on
// the holder from a non-party actor; (c) the party in combat (the player, or a teammate with a
// controller whose target is not the anchor). The trigger sets the slot's REAL bit FIRST: every
// deny above reads it and stands down on the combat thread at once. The game-thread pass then:
// restores the bit, removes the anchor from the group (CombatGroup::RemoveTarget, verified row:
// AE 44705 0x803D70 / SE 43483 0x76A560), releases the floors, drops the holder (the anchor goes
// home on 1 -> 0). NO StopCombat while the real foe is in the holder's group: the fight goes on,
// the client's cast claim is kept. marth 2026-10-13 (review F6, "End an empty fight"): when the
// group has NO target left once the anchor is removed (triggers (b) / (c): the foe was never in
// it), the fight is ended ONCE through the held engagement's ranked StopCombat
// (combatentry::hold::StopCombatRanked); the DROPPED line says which case it was.
//
// CLOSE (design 3.6): the claim's RELEASED fire (fire-and-forget). The census 0x0B NotifyStartCast
// seat (hand Charged -> Casting, the release REQUEST) only marks the window RELEASING; it closes as
// "fired" ONLY on the holder's TESSpellCastEvent for the driven form, as "released-unconfirmed"
// (a warning) on the hand back to None after a release state (4-6), and as "fired-not-released"
// (a warning) when neither comes within 3 s
// (fix/apmf-substrate-release-close, APMF-B93: closing on 0x0B itself freed the controller before
// the spell left the hand). Also: the claim's end (released, expired, the stop percent), the
// client's release, the TTL (param.fval seconds, 0 = [CombatSubstrate] fSubstrateTtlSec, clamped
// to 15 s; deferred while releasing), a caster built while the leaf gate never saw the holder's
// tree ("tree-outside-gate", a warning), the holder's death / unload, a load or revert.
// One pass: the bit restored, Actor::StopCombat
// once (Character slot 0xE5, the vtable identity checked), the floors released, the holder out of
// the slot table (the anchor home on 1 -> 0), EvaluatePackage(false, false). Prior behaviour
// returns because nothing else was written: the package was never replaced.
//
// LOAD (no co-save; the sweep below APPROVED by marth 2026-10-13, review F8). The holder set is game state only. A save written while a substrate was open
// carries the holder's controller and the anchor's position: AfterLoad sends the anchor home and,
// for a loaded actor whose group still lists the anchor, removes the anchor from that group
// (RemoveTarget) and, when nothing else is left for it to fight, ends that combat (StopCombat).
//
// THREADING. Open / serve / close / drop and every engine call run on the game thread (Poll,
// from Arbiter::OncePerFrame; StopCombat frees the controller inline, AE 0x6B7214, the same
// exposure as ch.21's range rule, APMF-B26). The combat-thread seats read ONLY the fixed slot
// table (atomics, a seqlock-style id re-check), never a controller. RemoveTarget takes no lock
// itself; the engine calls it unlocked from the main thread too (CombatManager AE 46875).
//
// THE DRY RUN ([CombatSubstrate] bSubstrateDryRun, P3 / P6). With no client yet (MFO's slice
// S3), Harbinger opens a substrate itself, with no claim and no cast, on every loaded, idle
// player teammate every fSubstrateDryRunPeriodSec, for fSubstrateTtlSec. Opt-in, default off;
// a test surface like core/Input.cpp, not a decision (principle 4 is about client facets).
// ============================================================================

namespace {

    using apmf::log::Hex;
    namespace subs = apmf::combatsubstrate;

    constexpr const char* kIni    = "Data/SKSE/Plugins/APMF.ini";
    constexpr const char* kPlugin = "APMF.esl";
    constexpr RE::FormID  kLocalAnchorNpc  = 0x808;
    constexpr RE::FormID  kLocalHoldingCell = 0x809;
    constexpr RE::FormID  kLocalAnchorRef  = 0x80A;
    constexpr RE::FormID  kLocalAnchorHome = 0x80B;

    constexpr int           kSlots          = 16;
    static_assert(kSlots <= apmf::substrateequip::kMaxSlots, "channels/SubstrateEquip.cpp tracks one record per slot");
    static_assert(kSlots <= apmf::substratebark::kMaxSlots, "channels/SubstrateBark.cpp tracks one record per slot");
    constexpr float         kMaxTtlSec      = 15.0f;   // design 3.6: the TTL is clamped to 15 s
    constexpr float         kFloorBasis     = -1.0e9f; // below any client basis: a client cast claim wins its hand
    constexpr std::uint64_t kFloorWaitMs    = 1000;    // the entry waits this long for the floors to publish
    constexpr std::uint64_t kTraceEveryMs   = 250;     // P6 MOVE cadence (design: once per 250 ms)
    constexpr std::uint64_t kPartyPollMs    = 250;     // trigger (c) poll
    constexpr std::uint64_t kHeartbeatMs    = 30000;
    constexpr float         kAnchorFollowU  = 32.0f;   // move the anchor when its holder moved this far
    constexpr std::uint16_t kTargetLost     = 1u << 1; // CombatTarget::flags kTargetLost (the hold's constant)
    constexpr std::uint64_t kRepickWaitMs   = 2000;    // review F2: the longest wait for the buffered re-pick (UNPAUSED time, R5 F-5)
    constexpr std::uint64_t kPollGapMs      = 500;     // R5 F-5: a longer gap between two Polls is a stall (not counted)
    constexpr std::uint64_t kFloorRenewMs   = 10000;   // review F5: renew the floors' 15 s windows this often
    // fix/apmf-ooc-substrate (principle 9, a floor is safe, an expiry is not): the TTL runs from the moment the
    // holder's behaviour tree first RUNS (its first node act() under the substrate), not from the open. Measured
    // 2026-10-20 (LoreRim town run 2, Auri, 5 windows): the tree started 2698 / 3283 / 3292 / 5043 / 3331 ms after the
    // open (each 0.2 to 2.5 s after the engine's first reset of the ignoring-combat bit). The old TTL ran from the open,
    // so a 6 s window left 1 to 3.3 s for the equip and the cast. Until the tree starts the substrate waits this
    // FLOOR (twice the longest start measured), then closes with its own reason (never a silent expiry).
    constexpr std::uint64_t kTreeStartWaitMs = 10000;
    // fix/apmf-substrate-release-close (APMF-B93, field-proven 2026-10-26, LoreRim v273, 3 of 3 windows): the census
    // 0x0B NotifyStartCast fires when the hand goes Charged -> Casting, i.e. the release was REQUESTED, not done. The
    // old close on it ran StopCombat 16-33 ms later and freed the controller before the spell left the hand (anim
    // SpellFire 0 every time). Now 0x0B only marks the window RELEASING; it closes as "fired" ONLY on the holder's
    // TESSpellCastEvent for the driven form. The hand back to None after a release state (4-6) closes as
    // "released-unconfirmed" (a warning: an interrupt reads the same; review F1 of 608df67).
    // The wait is bounded: MFO measured NotifyStartCast -> TESSpellCastEvent at 0.33 to 2.02 s on the same deck
    // (2026-10-26). 3 s is 1.5x the longest measured lag (a floor over the real cadence, principle 9); past it the
    // window closes LOUDLY as "fired-not-released" (principle 7), never silently as fired.
    constexpr std::uint64_t kReleaseWaitMs  = 3000;
    std::uint64_t           g_pollFrame     = 0;       // Poll calls (game thread)

    // Range tiers (design 3.3: the anchor stands at the holder whose claimed spell has the
    // SHORTEST range). Units are the reach a holder must be within for the "waits" line;
    // INFERRED, not measured (P3 measures how often a holder waits).
    enum Tier : int { kTierSelf = 0, kTierTouch = 1, kTierAimed = 2, kTierNone = 3 };
    constexpr float kTierReach[4] = { std::numeric_limits<float>::infinity(), 192.0f, 2048.0f,
                                      std::numeric_limits<float>::infinity() };
    constexpr const char* kTierName[4] = { "self", "touch", "aimed", "no claim" };

    // ---- slot table: the lock-free holder set (ANY THREAD reads) -------------------------
    enum : std::uint32_t {
        kStLive       = 1u << 0,   // in the holder set
        kStReal       = 1u << 1,   // real combat seen: every deny stands down
        kStCast       = 1u << 2,   // a live DRIVING cast claim: the three cast leaves run
        kStRankAction = 1u << 3,   // the substrate outranks Engine on CombatAction
        kStRankCombat = 1u << 4,   // ... on Combat (the anchor refusal)
        kStRankDetect = 1u << 5,   // ... on Detection (the anchor's knowledge)
        kStEntering   = 1u << 6,   // StartCombat(anchor) is running for this holder (diagnostic)
        kStEquipOk    = 1u << 7,   // a CLIENT substrate whose hands are stowed and whose spell-equip checks passed: the
                                   // kEquip leaves may run (with kStCast). Never set for a dry run (review of 1f99de2b)
    };
    enum : std::uint32_t { kWhyNone = 0, kWhyAddedTarget = 1, kWhyHit = 2, kWhyParty = 3 };
    constexpr const char* kWhyName[4] = { "?", "added-target", "hit", "party-combat" };

    struct Slot {
        std::atomic<RE::FormID>    id{ 0 };
        std::atomic<std::uint32_t> state{ 0 };
        std::atomic<std::uint32_t> holderH{ 0 };   // the holder's ActorHandle native value
        std::atomic<std::uint32_t> realWhy{ 0 };
        std::atomic<RE::FormID>    realFoe{ 0 };
        std::atomic<RE::FormID>    firedForm{ 0 };
        std::atomic<std::uint32_t> fires{ 0 };
        std::atomic<std::uint32_t> leafDenied{ 0 }, moveDenied{ 0 }, castAllowed{ 0 }, afterReal{ 0 },
            anchorRefused{ 0 }, unlost{ 0 }, combatEvents{ 0 }, hits{ 0 };
        std::array<std::atomic<std::uint32_t>, subs::kLeafCount> perLeaf{};
        // the music deny seat (kMus* bits, low byte = the first decided value) and when that value was first seen
        std::atomic<std::uint32_t> music{ 0 };
        std::atomic<std::uint64_t> musicAtMs{ 0 };
        // fix/apmf-ooc-substrate: when the holder's tree first ran under the substrate (ForLeaf, combat thread), the
        // movement hold's counters (core/ActionGate.cpp), the Combat Parallel's entries (1 = it kept running) and the
        // one claim-score line per window (core/AiCastSeats.cpp).
        std::atomic<std::uint64_t> treeStartMs{ 0 };
        std::atomic<std::uint32_t> combatParallelActs{ 0 }, holds{ 0 }, holdUpdates{ 0 }, holdReleased{ 0 },
            holdAnomalies{ 0 };
        std::atomic<bool>          claimScoreLogged{ false };
        // fix/apmf-substrate-release-close: the release evidence (SpellCastSink: the holder's TESSpellCastEvent for the
        // form its driving claim names, and when) and the deny-hole probe: ForLeaf's early returns for this holder by
        // the slot bit that failed (the first one's leaf and node), and a cast BUILT (census 0x06 YES) while the tree
        // had not started under the gate (the "tree-outside-gate" close).
        std::atomic<RE::FormID>    castEventForm{ 0 };
        std::atomic<std::uint64_t> castEventMs{ 0 };
        std::atomic<std::uint32_t> gateNotLive{ 0 }, gateReal{ 0 }, gateNoRank{ 0 }, gateBadIdx{ 0 };
        std::atomic<std::uint32_t> gateFirstLeaf{ 0xFFFFFFFFu }, gateFirstWhy{ 0 };
        std::atomic<const void*>   gateFirstNode{ nullptr };
        std::atomic<std::uint64_t> gateFirstMs{ 0 };
        std::atomic<RE::FormID>    builtOutside{ 0 };
        std::atomic<std::uint64_t> builtOutsideMs{ 0 };
        // fix/apmf-gate-probe-proxy-name: NoteCastBuilt's passive controller-identity line, once per window.
        std::atomic<bool>          builtProbeLogged{ false };
    };
    std::array<Slot, kSlots>   g_slots{};
    std::atomic<std::uint32_t> g_slotCount{ 0 };   // slots with an id (the AnyLive pre-gate)
    std::atomic<std::uint32_t> g_windowSerial{ 0 };   // bumped by TakeSlot (WindowSerial: ActionGate's probe)

    // seqlock-style read: the id, the state, the id again. A slot reused between the two id loads
    // reads as "not this actor".
    int FindSlot(RE::FormID a_fid, std::uint32_t& a_state) {
        if (a_fid == 0) return -1;
        for (int i = 0; i < kSlots; ++i) {
            if (g_slots[i].id.load(std::memory_order_acquire) != a_fid) continue;
            const std::uint32_t st = g_slots[i].state.load(std::memory_order_acquire);
            if (g_slots[i].id.load(std::memory_order_acquire) != a_fid) continue;
            a_state = st;
            return i;
        }
        return -1;
    }

    // ---- install-time state ----------------------------------------------------------------
    std::atomic<bool>        g_installed{ false };
    std::atomic<bool>        g_installTried{ false };
    std::atomic<const char*> g_notInstalledReason{ "before kDataLoaded (the substrate installs there)" };
    std::atomic<RE::FormID>  g_anchorId{ 0 };     // the ACHR
    std::atomic<std::uint32_t> g_anchorH{ 0 };    // its ActorHandle native value (game thread writes)
    RE::FormID               g_homeId = 0;        // the home XMarker
    RE::FormID               g_cellId = 0;
    std::uintptr_t           g_vtChar = 0;        // the verified Character vtable (StopCombat identity check)
    bool                     g_fireSignal = false;   // the census 0x0B seat is installed

    // INI
    bool          g_dryRun        = false;
    float         g_ttlSec        = 6.0f;
    float         g_dryPeriodSec  = 20.0f;
    bool          g_moveTrace     = false;
    bool          g_census        = true;   // [CombatSubstrate] bSubstrateBystanderCensus (passive; no deny)
    // fix/apmf-ooc-substrate: [CombatSubstrate] bSubstrateMovementHold (1): the Movement half of a held holder's
    // tree runs HELD (core/ActionGate.cpp "MOVEMENT HOLD") instead of force-failed. Read at Install; read by ForLeaf
    // and HoldsMovement on the combat thread (an atomic).
    std::atomic<bool> g_moveHold{ false };
    const char*       g_moveHoldOff = "";       // why it is off (Install only), for the per-OPEN hole line
    std::uint32_t     g_parallelIdx = 0xFFFFFFFFu;   // kLeaves index of CombatBehaviorParallel (Install only)

    using RemoveTarget_t = void (*)(RE::CombatGroup*, RE::TESObjectREFR*);
    using EvaluatePkg_t  = void (*)(RE::Actor*, bool, bool);
    REL::Relocation<RemoveTarget_t> g_removeTarget;
    REL::Relocation<EvaluatePkg_t>  g_evaluatePackage;

    // ---- leaf classification (built at Install from the catalog names) ----------------------
    // kEquip: the spell-equip path (channels/SubstrateEquip.cpp): runs only while a DRIVING cast claim stands,
    // and AcquireResource<2> only for its "Action" instance.
    enum class LeafClass : std::uint8_t { kDeny, kCast, kStructural, kEquip };
    std::array<LeafClass, subs::kLeafCount>   g_leafClass{};
    std::array<apmf::substrateequip::Node, subs::kLeafCount> g_equipNode{};
    std::array<const char*, subs::kLeafCount> g_leafName{};

    // ---- small helpers --------------------------------------------------------------------
    int IniInt(const char* a_sec, const char* a_key, int a_def) {
        char b[32]{};
        REX::W32::GetPrivateProfileStringA(a_sec, a_key, "", b, sizeof(b), kIni);
        if (!b[0]) return a_def;
        return std::atoi(b);
    }
    float IniFloat(const char* a_sec, const char* a_key, float a_def) {
        char b[32]{};
        REX::W32::GetPrivateProfileStringA(a_sec, a_key, "", b, sizeof(b), kIni);
        if (!b[0]) return a_def;
        const float v = std::strtof(b, nullptr);
        return std::isfinite(v) ? v : a_def;
    }

    bool GroupHasMember(const RE::CombatGroup* a_g, std::uint32_t a_handle) {
        if (!a_g || a_handle == 0) return false;
        for (const auto& m : a_g->members)
            if (m.memberHandle.native_handle() == a_handle) return true;
        return false;
    }

    RE::CombatTarget* FindTargetEntry(RE::CombatGroup* a_g, std::uint32_t a_handle) {
        if (!a_g || a_handle == 0) return nullptr;
        for (auto& t : a_g->targets)
            if (t.targetHandle.native_handle() == a_handle) return &t;
        return nullptr;
    }

    // The first group target that is not the anchor, or nullptr. Unlocked read: the caller is the
    // detection seat (the engine's own access there is unlocked, or under the group's write lock
    // on this thread) or holds the group's read lock.
    const RE::CombatTarget* FirstNonAnchorTarget(const RE::CombatGroup* a_g, std::uint32_t a_anchorH) {
        if (!a_g) return nullptr;
        for (const auto& t : a_g->targets) {
            const auto h = t.targetHandle.native_handle();
            if (h != 0 && h != a_anchorH) return &t;
        }
        return nullptr;
    }

    void MarkReal(int a_slot, std::uint32_t a_why, RE::FormID a_foe) {
        Slot&         s   = g_slots[a_slot];
        std::uint32_t exp = kWhyNone;
        if (s.realWhy.compare_exchange_strong(exp, a_why, std::memory_order_acq_rel))
            s.realFoe.store(a_foe, std::memory_order_relaxed);
        s.state.fetch_or(kStReal, std::memory_order_acq_rel);   // every deny reads this FIRST
    }

    bool HasController(RE::Actor* a) { return a && a->GetActorRuntimeData().combatController != nullptr; }

    RE::Actor* Anchor() {
        const auto id = g_anchorId.load(std::memory_order_relaxed);
        return id ? RE::TESForm::LookupByID<RE::Actor>(id) : nullptr;
    }

    // ---- game-thread state --------------------------------------------------------------------
    enum class Phase : std::uint8_t { kRequested, kFloorsQueued, kOpen };

    struct Sub {
        RE::FormID       id = 0;
        APMF_API::Handle handle = APMF_API::kInvalidHandle;   // the client's claim (0 = the dry run)
        bool             dryRun = false;
        Phase            phase = Phase::kRequested;
        int              slot = -1;
        std::uint64_t    requestedMs = 0, queuedMs = 0, openedMs = 0, deadlineMs = 0, ttlMs = 0;
        APMF_API::Handle floorR = APMF_API::kInvalidHandle, floorL = APMF_API::kInvalidHandle;
        bool             bitSet = false, priorBit = false;   // priorBit: the bit just before Harbinger's write
        bool             entryBit = false;                    // the bit at the entry (a pre-set bit = a paused controller)
        // review F2 of a512763: the single write waits for the re-pick it requested (see WriteBitAfterRepick)
        bool             bitPending = false;
        std::uint64_t    repickAskedMs = 0, repickClearMs = 0;
        std::uint64_t    repickClearFrame = 0;
        // R5 F-5: the re-pick wait counts only time the game ran (no pausing menu, no stalled frame)
        std::uint64_t    repickLastPollMs = 0, repickRunMs = 0, repickPausedMs = 0, repickGapMs = 0;
        RE::TESPackage*  bitPkg = nullptr;          // the running package when the bit was written
        float            bitPkgStart = 0.0f;        // its ActorPackage::packageStartTime then
        std::uint32_t    resetsByRestart = 0, resetsOther = 0;
        // review F5: the floors are renewed while the substrate lives
        std::uint64_t    floorRenewMs = 0;
        // review F3: the anchor's process level, seen while this substrate was open
        bool             anchorHighSeen = false;
        bool             closeAsked = false;       // the client released (Release runs inside Drain)
        // the spell equip (review of 1f99de2b): the client checks + Stow were made (at Begin, or at a dry-run takeover in
        // Serve), and kStEquipOk was set once StowSettled read the hands clear
        bool             stowAsked = false, equipOk = false;
        const char*      closeWhy = nullptr;
        // the claim the substrate serves
        bool             sawClaim = false;
        RE::FormID       driven = 0;
        bool             concentration = false;
        int              tier = kTierNone;
        // P6
        std::uint64_t    lastTraceMs = 0, lastPosMs = 0;
        RE::NiPoint3     lastPos{};
        std::uint32_t    bitResets = 0;
        std::uint64_t    firstResetMs = 0;
        bool             bitWasOn = false;
        // P3 observed
        bool             playerCombatSeen = false;
        bool             aggressorSeen = false;
        // the music probe (ClickUp 86e3pu939): the group's first decided state comes from the slot (the seat reads
        // it inside the engine's own update of the group); the BSMusicManager track at open / every change
        bool             musicDecidedLogged = false;
        RE::FormID       musicAtOpen = 0;
        std::uint32_t    musicChanges = 0;         // BSMusicManager changes while this substrate was open
        bool             combatEventLogged = false;
        RE::FormID       waitLoggedFor = 0;
        // fix/apmf-ooc-substrate: the tree's first run (the TTL runs from it) and the movement hold's first line
        bool             treeStarted = false, holdLogged = false;
        std::uint64_t    treeStartAt = 0;
        // fix/apmf-substrate-release-close (APMF-B93): 0x0B NotifyStartCast on the driven form marks the window
        // RELEASING (the TTL defers to kReleaseWaitMs from here); the hand's highest state seen while releasing
        // (4-6, a release state, arms the "back to None" close, released-unconfirmed); how it closed (nullptr = not yet)
        bool             releasing = false, releasePeak = false;
        std::uint64_t    releaseAtMs = 0;
        RE::FormID       releaseForm = 0;
        std::uint32_t    releaseHandMax = 0;
        const char*      releaseHow = nullptr;
    };
    std::unordered_map<RE::FormID, Sub> g_subs;          // GAME THREAD ONLY
    std::atomic<std::size_t>            g_subCount{ 0 }; // Poll pre-gate (relaxed)

    // The holder the anchor stands at. Written on the game thread; READ by RefuseAnchorTarget on the combat /
    // detection threads for its log attribution (review F4 of a512763): an atomic, seq_cst by default.
    std::atomic<RE::FormID> g_served{ 0 };
    std::uint32_t g_anchorOut = 0;            // holders the anchor is out for (game thread)
    std::uint64_t g_lastDryMs = 0, g_lastPartyMs = 0, g_lastHeartbeatMs = 0;
    // heartbeat totals (game thread)
    std::uint64_t g_opened = 0, g_closed = 0, g_drops = 0, g_refusedOpen = 0, g_entryRefused = 0;
    std::atomic<std::uint64_t> g_anchorRefusals{ 0 };
    std::atomic<std::uint64_t> g_anchorRefusalLastLogMs{ 0 };

    std::uint64_t TtlMsFor(float a_sec) {
        float s = a_sec > 0.0f && std::isfinite(a_sec) ? a_sec : g_ttlSec;
        if (!(s > 0.0f)) s = 6.0f;
        if (s > kMaxTtlSec) s = kMaxTtlSec;
        return static_cast<std::uint64_t>(std::llround(static_cast<double>(s) * 1000.0));
    }

    // The substrate's client (or, with no handle, the client default) outranks Engine on `f`?
    bool Outranks(APMF_API::Handle a_h, apmf::sources::Facet a_f) {
        return apmf::sources::ClaimPriority(a_h, a_f) > apmf::sources::Priority(apmf::sources::kEngineSource, a_f);
    }

    // The user's priority sliders decide (marth: "Nothing escapes Harbinger"). A deny that would not stand leaves a
    // facet the substrate switched on undenied: then it does not open. Begin, and a dry-run TAKEOVER in Serve (review
    // of 1f99de2b) before the spell equip is allowed. nullptr = it stands.
    const char* RankRefusal(const Sub& a_sub);

    // The delivery tier of a driving claim's spell (design 3.3's "shortest range").
    int TierOf(RE::FormID a_spell) {
        auto* sp = a_spell ? RE::TESForm::LookupByID<RE::SpellItem>(a_spell) : nullptr;
        if (!sp) return kTierAimed;
        switch (sp->GetDelivery()) {
        case RE::MagicSystem::Delivery::kSelf:  return kTierSelf;
        case RE::MagicSystem::Delivery::kTouch: return kTierTouch;
        default:                                 return kTierAimed;
        }
    }

    // The driving cast claim on `a_id` (not a deny-only floor), if any.
    bool DrivingClaim(RE::FormID a_id, apmf::CastSeatClaim& a_out) {
        if (!apmf::ControlMap::Get().TryGetCastSeatClaim(a_id, a_out)) return false;
        if (a_out.flags & APMF_API::kCastFlag_DenyHandOnly) return false;
        return a_out.spell != 0 || a_out.proxy != 0;
    }

    // fix/apmf-substrate-release-close: the highest MagicCaster state of the two HANDS (L, R) whose current spell is
    // the claim's spell or proxy; 0 when neither hand holds it (state None, or the hand moved on / cleared its spell).
    // GAME THREAD. READ the slot, never Actor::GetMagicCaster (Character slot 0x5C ALLOCATES a caster on a null slot);
    // the same read as core/CasterTypeCensus.cpp HandHolds and core/CastObserve.cpp, whose static_asserts pin
    // ACTOR_RUNTIME_DATA::magicCasters at +0xC0 (the disassembled +0x1A8 AE / +0x1A0 SE of GetMagicCaster).
    // magicCasters[] is indexed by MagicSystem::CastingSource (kLeftHand 0, kRightHand 1). State numbers are the
    // field-observed ones (1 Unk1, 2 Charging, 3 Charged, 4 Casting, 5 Released, 6 Concluding).
    std::uint32_t HandStateFor(RE::Actor* a_actor, RE::FormID a_spell, RE::FormID a_proxy) {
        if (!a_actor) return 0;
        std::uint32_t best = 0;
        for (const auto src : { RE::MagicSystem::CastingSource::kLeftHand, RE::MagicSystem::CastingSource::kRightHand }) {
            auto* mc = a_actor->GetActorRuntimeData().magicCasters[static_cast<std::size_t>(src)];
            if (!mc || !mc->currentSpell) continue;
            const RE::FormID f = mc->currentSpell->GetFormID();
            if (f != a_spell && (a_proxy == 0 || f != a_proxy)) continue;
            best = (std::max)(best, static_cast<std::uint32_t>(mc->state.get()));
        }
        return best;
    }

    // ForLeaf's early returns, by the slot bit that failed (the LEAF GATE line names them)
    enum : std::uint32_t { kGateNotLive = 1, kGateReal = 2, kGateNoRank = 3, kGateBadIdx = 4 };
    const char* GateWhyName(std::uint32_t a_why) {
        switch (a_why) {
        case kGateNotLive: return "kStLive clear";
        case kGateReal:    return "kStReal set (the real-combat stand-down)";
        case kGateNoRank:  return "kStRankAction clear";
        case kGateBadIdx:  return "leaf index out of range";
        default:           return "-";
        }
    }

    void ClearSlot(int a_slot) {
        if (a_slot < 0 || a_slot >= kSlots) return;
        Slot& s = g_slots[a_slot];
        s.state.store(0, std::memory_order_release);
        s.id.store(0, std::memory_order_release);
        g_slotCount.fetch_sub(1, std::memory_order_relaxed);
    }

    int TakeSlot(RE::FormID a_id, std::uint32_t a_holderH, std::uint32_t a_state) {
        for (int i = 0; i < kSlots; ++i) {
            Slot& s = g_slots[i];
            if (s.id.load(std::memory_order_acquire) != 0) continue;
            // Reset everything before the id publishes the slot (a combat-thread reader that matches
            // the id afterwards sees the fresh counters).
            s.state.store(0, std::memory_order_relaxed);
            s.holderH.store(a_holderH, std::memory_order_relaxed);
            s.realWhy.store(kWhyNone, std::memory_order_relaxed);
            s.realFoe.store(0, std::memory_order_relaxed);
            s.firedForm.store(0, std::memory_order_relaxed);
            s.fires.store(0, std::memory_order_relaxed);
            for (auto* c : { &s.leafDenied, &s.moveDenied, &s.castAllowed, &s.afterReal, &s.anchorRefused,
                             &s.unlost, &s.combatEvents, &s.hits })
                c->store(0, std::memory_order_relaxed);
            for (auto& c : s.perLeaf) c.store(0, std::memory_order_relaxed);
            s.music.store(0, std::memory_order_relaxed);
            s.musicAtMs.store(0, std::memory_order_relaxed);
            s.treeStartMs.store(0, std::memory_order_relaxed);
            for (auto* c : { &s.combatParallelActs, &s.holds, &s.holdUpdates, &s.holdReleased, &s.holdAnomalies })
                c->store(0, std::memory_order_relaxed);
            s.claimScoreLogged.store(false, std::memory_order_relaxed);
            s.castEventForm.store(0, std::memory_order_relaxed);
            s.castEventMs.store(0, std::memory_order_relaxed);
            for (auto* c : { &s.gateNotLive, &s.gateReal, &s.gateNoRank, &s.gateBadIdx, &s.gateFirstWhy })
                c->store(0, std::memory_order_relaxed);
            s.gateFirstLeaf.store(0xFFFFFFFFu, std::memory_order_relaxed);
            s.gateFirstNode.store(nullptr, std::memory_order_relaxed);
            s.gateFirstMs.store(0, std::memory_order_relaxed);
            s.builtOutside.store(0, std::memory_order_relaxed);
            s.builtOutsideMs.store(0, std::memory_order_relaxed);
            s.builtProbeLogged.store(false, std::memory_order_relaxed);
            g_windowSerial.fetch_add(1, std::memory_order_relaxed);
            s.id.store(a_id, std::memory_order_release);
            s.state.store(a_state, std::memory_order_release);
            g_slotCount.fetch_add(1, std::memory_order_relaxed);
            return i;
        }
        return -1;
    }

    // ---- the anchor (game thread) ---------------------------------------------------------------
    // AI off (boolBits kProcessMe), invisible (SetAlpha 0), kAnchorBehindU BEHIND the holder (its heading).
    // COLLISION: not switched off. The only setter (CommonLib TESObjectREFR::SetCollision) writes form flag
    // 1 << 4, which CommonLib itself marks unverified ("?"), and this file writes no unverified engine state
    // (CLAUDE.md rule 2). Standing behind the holder, outside its capsule, keeps the anchor's body out of the
    // follower's way instead; it is a ghost (ACBS) so nothing hits it.
    constexpr float kAnchorBehindU = 72.0f;
    RE::NiPoint3 BehindOf(RE::Actor* a_at) {
        const RE::NiPoint3 p = a_at->GetPosition();
        const float        z = a_at->data.angle.z;   // heading: forward = (sin z, cos z)
        return RE::NiPoint3{ p.x - std::sin(z) * kAnchorBehindU, p.y - std::cos(z) * kAnchorBehindU, p.z };
    }

    void AnchorToHolder(RE::Actor* a_anchor, RE::Actor* a_at, bool a_first) {
        if (!a_anchor || !a_at) return;
        if (a_first) a_anchor->GetActorRuntimeData().boolBits.reset(RE::Actor::BOOL_BITS::kProcessMe);   // AI off
        if (a_anchor->GetParentCell() != a_at->GetParentCell() || a_first)
            a_anchor->MoveTo(a_at);   // into the holder's cell (MoveTo_Impl, the verified row) ...
        // ... then behind it: TESObjectREFR::SetPosition (MoveTo_Impl), NOT Actor's slot 0xA9 overload.
        static_cast<RE::TESObjectREFR*>(a_anchor)->SetPosition(BehindOf(a_at));
        a_anchor->SetAlpha(0.0f);   // Character 0xE1: a no-op until the anchor is high process -- re-applied per move
    }

    void AnchorHome(RE::Actor* a_anchor, const char* a_why) {
        if (!a_anchor) return;
        auto* home = g_homeId ? RE::TESForm::LookupByID<RE::TESObjectREFR>(g_homeId) : nullptr;
        if (!home) {
            spdlog::error("[substrate] anchor NOT sent home ({}): the home marker 0x{} does not resolve.", a_why,
                          Hex(g_homeId));
            return;
        }
        a_anchor->MoveTo(home);
        g_served = 0;
    }

    std::uint32_t LiveHolders() {
        std::uint32_t n = 0;
        for (const auto& s : g_slots)
            if (s.id.load(std::memory_order_acquire) != 0 &&
                (s.state.load(std::memory_order_acquire) & (kStLive | kStReal)) == kStLive)
                ++n;
        return n;
    }

    std::uint32_t PackageOf(RE::Actor* a, std::uint32_t* a_type = nullptr) {
        auto* p = a ? a->GetActorRuntimeData().currentProcess : nullptr;
        auto* pkg = p ? p->currentPackage.package : nullptr;
        if (a_type) *a_type = pkg ? static_cast<std::uint32_t>(pkg->packData.packType.get()) : 0xFFu;
        return pkg ? pkg->GetFormID() : 0;
    }

    RE::FormID MusicNow() {   // only after the BSMusicManager singleton row verified (g_musicOk)
        auto* mm = RE::BSMusicManager::GetSingleton();
        if (!mm || !mm->current) return 0;
        auto* mt = skyrim_cast<RE::BGSMusicType*>(mm->current);
        return mt ? mt->GetFormID() : 0;
    }
    bool g_musicOk = false;   // the BSMusicManager singleton row verified

    bool PlayerInCombat() {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        return pc && pc->IsInCombat();   // PlayerCharacter slot 0xE3 (AE 0x758430: +0xBE3 bit 5)
    }

    // ---- the music probe (ClickUp 86e3pu939, review of the 2026-10-13 dry run) ---------------------
    // Every BSMusicManager current-track change is logged while a substrate is open and for kMusicTailMs after
    // the last one closes (marth heard battle music start with the HUD's combat icon and cycle back to
    // exploration as it ended; a close-time sample saw only the cycled-back track). Game thread only.
    constexpr std::uint64_t kMusicTailMs = 15000;
    bool          g_musicTracking   = false;
    RE::FormID    g_musicLast       = 0;
    std::uint64_t g_musicTrackUntil = 0;   // the tail's end (set while a substrate is open)
    std::uint64_t g_musicLastOpenMs = 0;   // the last Poll a substrate was open

    // ---- the music deny (ClickUp 86e3pu939; marth 2026-10-13: substrate combat is not combat) ---------
    // CombatGroup+0x15C is the group's music decision. The ctor writes 0xFF (AE 0x8033D5 / SE 0x769F5D). The
    // group update (AE 44703 / SE 43481) takes the group's WRITE lock at +0x160 first and holds it to its end (row
    // Substrate.CombatGroup.MusicWriteLock); it decides +0x15C ONLY while it reads 0xFF (row
    // Substrate.CombatGroup.MusicUndecided): 1.0 s after its first update it writes 0, then 1 when the group's
    // strength ratio is in band (0.5 .. 2.0). CombatManager's should-play test (AE 46888 / SE 45588) lets a group
    // that does not target the player drive battle music only while +0x15C == 1 (row
    // Substrate.CombatManager.MusicPlayTest; "Path B"). A PURE-substrate group (every member a live holder, every
    // target the anchor) does not target the player, so writing its undecided 0xFF to 0 before the engine decides
    // leaves it no road to battle music.
    // THE MERGE (tier-3 review F1 of b1c78b2): CombatGroup::Merge (AE 44722 0x804A80, dest rcx, src rdx) copies the
    // SOURCE's decided +0x15C over the destination's (AE 0x804F18), even over an engine-decided 1. Its only caller is
    // StartCombat's join path (AE 0x83D325: Merge(join group, the actor's existing group), then the old group is
    // retired, AE 0x8048D0). So a holder on a denied group that is pulled into an ally's real fight carries the 0
    // INTO that fight's group. That is why the 0 is owed per HOLDER (the debt table below), never per group pointer:
    // whatever group a debtor is a member of when a real (non-anchor) target is seen in it, Harbinger's 0 there is
    // put back to 0xFF.
    // WHERE (review F6 of b1c78b2): CombatGroup has no reference count (dtor AE 44702 0x8034B0; freed by AE 46870
    // 0x83CB20 from the main update AE 36564 and the "Non render-safe AI" job AE 39089 -> 36592), and the group
    // update runs on the "Combat manager" job (AE 39044 -> 46869 0x83CA30, no manager lock). A group pointer read
    // from a controller on the game thread has no lifetime guarantee. So every read and write of +0x15C happens
    // INSIDE the engine's own update of that group: the held engagement's detection seat (channels/CombatEntry.cpp
    // DetectThunk) is called from the group detection pass (AE 44804, called at 44703+0x6D; SE 43572 at 43481+0x65,
    // row Substrate.CombatGroup.DetectionPassInUpdate), after the update's LockForWrite (+0x30 / +0x2B) and before its
    // music decision (+0x23C / +0x94). The seat acts only when THIS thread holds the group's write lock
    // (WriteLockedHere: the lock's +0 writer thread id is GetCurrentThreadId and +4 has the writer bit, the layout the
    // CommonLib.BSReadWriteLock.LockForWrite / UnlockForWrite rows pin), i.e. only inside the engine's own locked use
    // of a live group. Timing: the detection pass first runs 0.5 s after the group's ctor (AE ctor 0x8033F0/0x8033F8:
    // +0xB0 = now, +0xB4 = 0.5), the music decision 1.0 s after the first update (+0xB8 / +0xBC start at 0), and in
    // one update the pass runs before the decision.
    constexpr std::uint8_t kMusicUndecided = 0xFF;
    constexpr std::uint8_t kMusicNone      = 0;
    bool        g_musicDeny    = false;
    const char* g_musicDenyOff = "not installed";

    // Every member a live holder still under its substrate, every target the anchor, at least one of each.
    // The caller holds the group's lock.
    bool PureSubstrateGroup(const RE::CombatGroup* a_g, std::uint32_t a_anchorH, std::uint32_t& a_members,
                            std::uint32_t& a_targets) {
        a_members = a_g->members.size();
        a_targets = a_g->targets.size();
        if (a_anchorH == 0 || a_members == 0 || a_targets == 0) return false;
        for (const auto& t : a_g->targets)
            if (t.targetHandle.native_handle() != a_anchorH) return false;
        for (const auto& m : a_g->members) {
            const auto h = m.memberHandle.native_handle();
            bool       holder = false;
            for (const auto& s : g_slots) {
                if (s.id.load(std::memory_order_acquire) == 0 || s.holderH.load(std::memory_order_relaxed) != h) continue;
                holder = (s.state.load(std::memory_order_acquire) & (kStLive | kStReal)) == kStLive;
                break;
            }
            if (!holder) return false;
        }
        return true;
    }

    std::string MusicName(std::uint8_t a_state) {
        return a_state == kMusicUndecided ? std::string("0xFF (undecided)")
             : a_state == kMusicNone      ? std::string("0 (no battle music from this group)")
             : a_state == 1               ? std::string("1 (this group can drive battle music, Path B)")
                                          : fmt::format("{} (UNKNOWN value)", a_state);
    }

    // ---- the music deny SEAT (review F1 / F2 / F4 / F6 of b1c78b2; see "the music deny" above) -----------
    // Slot::music bits: the low byte = the FIRST decided value this holder's group showed, kMusSeen = it was
    // seen; kMusWrote = Harbinger's 0 was written for (or shared by) this holder's group; kMusRestored = a group
    // holding this holder and a real target had Harbinger's 0 put back to 0xFF; the two log-once bits.
    constexpr std::uint32_t kMusSeen = 1u << 8, kMusWrote = 1u << 9, kMusRestored = 1u << 10,
                            kMusWaitLogged = 1u << 11, kMusMissed = 1u << 12;

    // True only while THIS thread holds `a_g`'s write lock (BSReadWriteLock: +0 the writer's thread id, +4 the
    // count with the writer bit 0x80000000; AE LockForWrite 0xCC914D calls GetCurrentThreadId and compares [rcx]).
    bool WriteLockedHere(const RE::CombatGroup* a_g) {
        const auto* w = reinterpret_cast<const volatile std::uint32_t*>(&a_g->lock);
        return w[0] != 0 && w[0] == REX::W32::GetCurrentThreadId() && (w[1] & 0x80000000u) != 0;
    }

    // THE DEBT TABLE: a holder whose group carries Harbinger's 0 owes a put-back to whatever group it is a member of
    // when that group shows a real target (the merge, review F1). One entry per HOLDER (every member at the write,
    // and a co-holder that joins a denied group, review F2), matched by membership in the group the engine is
    // updating, never by a stored group pointer (review F4). expiresMs 0 = the substrate is open; a drop gives the
    // entry kDebtAfterDropMs; a normal close forgets it (StopCombat ends the holder's part in the group).
    struct MusicDebt {
        std::uint32_t holderH = 0;
        RE::FormID    holder = 0;
        std::uint64_t expiresMs = 0;
    };
    constexpr std::size_t   kDebts           = 32;
    constexpr std::uint64_t kDebtAfterDropMs = 30000;
    std::mutex                    g_debtMx;   // LEAF: no engine call, no log, no other lock under it
    std::array<MusicDebt, kDebts> g_debts{};
    std::atomic<std::uint32_t>    g_debtCount{ 0 };   // entries in use (the seat's pre-gate)

    void DebtCountLocked() {
        std::uint32_t n = 0;
        for (const auto& d : g_debts) n += d.holderH != 0;
        g_debtCount.store(n, std::memory_order_release);
    }
    bool DebtSet(std::uint32_t a_holderH, RE::FormID a_holder, std::uint64_t a_expires) {
        std::scoped_lock lk(g_debtMx);
        MusicDebt* free = nullptr;
        for (auto& d : g_debts) {
            if (d.holderH == a_holderH) { d.holder = a_holder; d.expiresMs = a_expires; return true; }
            if (!free && d.holderH == 0) free = &d;
        }
        if (!free) return false;
        *free = MusicDebt{ a_holderH, a_holder, a_expires };
        DebtCountLocked();
        return true;
    }
    void DebtTimeout(RE::FormID a_holder, std::uint64_t a_expires) {
        std::scoped_lock lk(g_debtMx);
        for (auto& d : g_debts)
            if (d.holderH != 0 && d.holder == a_holder) d.expiresMs = a_expires;
    }
    void DebtForget(RE::FormID a_holder) {
        std::scoped_lock lk(g_debtMx);
        for (auto& d : g_debts)
            if (d.holderH != 0 && d.holder == a_holder) d = MusicDebt{};
        DebtCountLocked();
    }

    std::string SlotIds(const std::array<int, kSlots>& a_idx, int a_n) {
        std::string out;
        for (int k = 0; k < a_n; ++k)
            out += fmt::format("{}0x{}", out.empty() ? "" : ",", Hex(g_slots[a_idx[k]].id.load(std::memory_order_relaxed)));
        return out;
    }

    // From AnchorKnowledge (the seat saw the anchor in `a_g`). Inside the engine's locked update only.
    void SeatMusicAnchor(RE::CombatGroup* a_g) {
        if (!WriteLockedHere(a_g)) return;
        std::array<int, kSlots> mine{};
        int                     n = 0;
        bool                    anyWrote = false;
        for (const auto& m : a_g->members) {
            const auto h = m.memberHandle.native_handle();
            for (int i = 0; i < kSlots && n < kSlots; ++i) {
                if (g_slots[i].id.load(std::memory_order_acquire) == 0 ||
                    g_slots[i].holderH.load(std::memory_order_relaxed) != h)
                    continue;
                if (g_slots[i].state.load(std::memory_order_acquire) & kStLive) {
                    mine[n++] = i;
                    if (g_slots[i].music.load(std::memory_order_acquire) & kMusWrote) anyWrote = true;
                }
                break;
            }
        }
        if (n == 0) return;
        const std::uint8_t ms = a_g->musicState;
        std::uint32_t      members = 0, targets = 0;
        const bool         pure = PureSubstrateGroup(a_g, g_anchorH.load(std::memory_order_acquire), members, targets);
        bool               wrote = false, coOwned = false;
        if (g_musicDeny && pure) {
            if (ms == kMusicUndecided) {
                a_g->musicState = kMusicNone;
                wrote           = true;
            } else if (ms == kMusicNone && anyWrote) {
                coOwned = true;   // review F2: a holder that joined a group Harbinger already denied owes it too
            }
        }
        const std::uint8_t seen = wrote ? kMusicNone : ms;
        const auto         now  = apmf::clock::MonotonicMs();
        bool               logWait = false, logMiss = false, logCo = false;
        for (int k = 0; k < n; ++k) {
            Slot&               s    = g_slots[mine[k]];
            const std::uint32_t prev = s.music.load(std::memory_order_acquire);
            std::uint32_t       add  = 0;
            if ((wrote || coOwned) && !(prev & kMusWrote)) {
                add |= kMusWrote;
                DebtSet(s.holderH.load(std::memory_order_relaxed), s.id.load(std::memory_order_relaxed), 0);
                if (coOwned) logCo = true;
            }
            if (seen != kMusicUndecided && !(prev & kMusSeen)) {
                s.musicAtMs.store(now, std::memory_order_relaxed);
                add |= kMusSeen | seen;
            }
            if (g_musicDeny && !wrote && !coOwned && !((prev | add) & kMusWrote)) {
                if (ms == kMusicUndecided && !pure && !(prev & kMusWaitLogged)) { add |= kMusWaitLogged; logWait = true; }
                if (ms != kMusicUndecided && !(prev & kMusMissed)) { add |= kMusMissed; logMiss = true; }
            }
            if (add) s.music.fetch_or(add, std::memory_order_acq_rel);
        }
        try {
            if (wrote)
                spdlog::info("[substrate] MUSIC DENIED: group of {} -- its music decision (CombatGroup+0x15C) 0xFF -> 0 inside "
                             "the engine's own locked update (pure substrate: {} member(s), all live holders; {} target(s), "
                             "all the anchor). Owed back to 0xFF by each holder to any group of theirs that shows a real target.",
                             SlotIds(mine, n), members, targets);
            if (logCo)
                spdlog::info("[substrate] music deny: {} joined a group Harbinger already denied; co-owner of its 0 (review F2).",
                             SlotIds(mine, n));
            if (logWait)
                spdlog::warn("[substrate] music deny WAITS: the group of {} is not pure-substrate ({} member(s), {} target(s): "
                             "a member that is not a live holder or a target that is not the anchor), so Harbinger does "
                             "not touch it. Retried on every detection pass while its music is undecided.",
                             SlotIds(mine, n), members, targets);
            if (logMiss)
                spdlog::warn("[substrate] music deny MISSED: the group of {} already reads {} (not Harbinger's write); "
                             "Harbinger writes only an UNDECIDED (0xFF) group and did not touch it.",
                             SlotIds(mine, n), MusicName(ms));
        } catch (...) {
        }
    }

    // From NoteGroupTarget (the seat saw a target that is NOT the anchor in `a_g`: a real target). Inside the
    // engine's locked update only. Any debtor that is a member of this group gets Harbinger's 0 put back to 0xFF
    // (only a 0: an engine 1 or an undecided group is left alone), so the engine decides this real fight's music.
    void SeatMusicRealTarget(RE::CombatGroup* a_g, RE::TESObjectREFR* a_target) {
        if (g_debtCount.load(std::memory_order_acquire) == 0 || !WriteLockedHere(a_g)) return;
        if (a_g->musicState != kMusicNone) return;
        std::array<RE::FormID, kDebts>    who{};
        std::array<std::uint32_t, kDebts> whoH{};
        int                               n = 0;
        {
            std::scoped_lock lk(g_debtMx);
            for (auto& d : g_debts) {
                if (d.holderH == 0) continue;
                for (const auto& m : a_g->members)
                    if (m.memberHandle.native_handle() == d.holderH) {
                        who[n]  = d.holder;
                        whoH[n] = d.holderH;
                        ++n;
                        d = MusicDebt{};
                        break;
                    }
            }
            if (n) DebtCountLocked();
        }
        if (n == 0) return;
        a_g->musicState = kMusicUndecided;
        for (int k = 0; k < n; ++k)
            for (auto& s : g_slots)
                if (s.id.load(std::memory_order_acquire) != 0 && s.holderH.load(std::memory_order_relaxed) == whoH[k])
                    s.music.fetch_or(kMusRestored, std::memory_order_acq_rel);
        try {
            std::string ids;
            for (int k = 0; k < n; ++k) ids += fmt::format("{}0x{}", ids.empty() ? "" : ",", Hex(who[k]));
            spdlog::info("[substrate] MUSIC RESTORED: a group holding {} shows a real target 0x{} and carried Harbinger's 0 "
                         "(its own denied group, or merged into an ally's fight, review F1): 0 -> 0xFF inside the engine's "
                         "locked update; the engine decides this fight's music now.",
                         ids, Hex(a_target ? a_target->GetFormID() : 0));
        } catch (...) {
        }
    }

    // ---- logs ----------------------------------------------------------------------------------
    std::string LeafCounts(const Slot& a_s) {
        std::string out;
        for (std::uint32_t i = 0; i < subs::kLeafCount; ++i) {
            const auto n = a_s.perLeaf[i].load(std::memory_order_relaxed);
            if (!n || !g_leafName[i]) continue;
            std::string_view nm = g_leafName[i];
            if (nm.starts_with("CombatBehavior")) nm.remove_prefix(14);
            out += fmt::format("{}{}={}", out.empty() ? "" : ",", nm, n);
        }
        return out.empty() ? "none" : out;
    }

    void LogObserved(const Sub& a_sub, RE::Actor* a_actor) {
        const Slot& s = g_slots[a_sub.slot];
        const std::string music =
            g_musicOk ? fmt::format("at open 0x{}, now 0x{}, {} change(s) while open (each one is a MUSIC line)",
                                    Hex(a_sub.musicAtOpen), Hex(MusicNow()), a_sub.musicChanges)
                      : std::string("n/a (the BSMusicManager row did not verify)");
        const std::uint32_t mu  = s.music.load(std::memory_order_acquire);
        const std::uint64_t at  = s.musicAtMs.load(std::memory_order_relaxed);
        const std::string decided =
            (mu & kMusSeen)
                ? fmt::format("{} at +{} ms by {}", MusicName(static_cast<std::uint8_t>(mu & 0xFF)),
                              at > a_sub.openedMs ? at - a_sub.openedMs : 0,
                              (mu & kMusWrote) && (mu & 0xFF) == kMusicNone ? "Harbinger's music deny" : "the ENGINE")
                : std::string("never decided while open (0xFF)");
        spdlog::info("[substrate] 0x{} OBSERVED music={} | group music state {} | music deny {} | player-in-combat={} "
                     "aggressor-bit={} ignoring-readers=not "
                     "instrumented (8 engine call sites per runtime; a raw call-site probe is forbidden by INVARIANTS "
                     "#17, see Docs/FACETS.md 'Combat substrate') | anchor StartCombat refused {} | anchor un-lost {} | "
                     "hits on the holder {} | casts released {} | anchor reached high process {} | ignoring-bit resets "
                     "{} (package restart {}, other {})",
                     Hex(a_sub.id), music, decided,
                     !g_musicDeny            ? fmt::format("OFF ({})", g_musicDenyOff)
                     : (mu & kMusRestored)   ? std::string("WROTE 0, then PUT BACK to 0xFF (a real target in a group holding him)")
                     : (mu & kMusWrote)      ? std::string("WROTE 0 for his group (no real target seen in a group holding him since)")
                     : (mu & kMusMissed)     ? std::string("did not write: MISSED (the group was decided first, see its line)")
                     : (mu & kMusWaitLogged) ? std::string("did not write: the group was never pure-substrate (WAITS line)")
                                             : std::string("did not write (no detection pass of his group while open)"),
                     a_sub.playerCombatSeen ? "y" : "n", a_sub.aggressorSeen ? 1 : 0,
                     s.anchorRefused.load(std::memory_order_relaxed), s.unlost.load(std::memory_order_relaxed),
                     s.hits.load(std::memory_order_relaxed), s.fires.load(std::memory_order_relaxed),
                     a_sub.anchorHighSeen ? "y" : "NO (SetAlpha never applied: the anchor stayed visible)",
                     a_sub.bitResets, a_sub.resetsByRestart, a_sub.resetsOther);
        (void)a_actor;
    }

    // ---- open ------------------------------------------------------------------------------------
    // Ends a substrate that never entered (or the client's claim when the substrate cannot exist).
    const char* RankRefusal(const Sub& a_sub) {
        using F = apmf::sources::Facet;
        // kCast (review F3 of a0112b24): the floored hand's spell refusal (core/EquipGate.cpp deny-complete) and
        // the 0x0A CheckCast refusal rank on the Cast facet; with the spell equip open they are load-bearing.
        for (const auto f : { F::kCombatAction, F::kCombat, F::kEquipWeapon, F::kDetection, F::kCast })
            if (!Outranks(a_sub.handle, f))
                return f == F::kCombatAction ? "Engine outranks the substrate's client on the CombatAction facet (APMF_Sources.ini): the leaf allow-list would not stand"
                     : f == F::kCombat       ? "Engine outranks the substrate's client on the Combat facet (APMF_Sources.ini): the anchor refusal would not stand"
                     : f == F::kEquipWeapon  ? "Engine outranks the substrate's client on the EquipWeapon facet (APMF_Sources.ini): the hand floors would not stand"
                     : f == F::kCast         ? "Engine outranks the substrate's client on the Cast facet (APMF_Sources.ini): the floored hand's spell refusal (0x0F deny-complete) and the 0x0A cast refusal would not stand"
                                             : "Engine outranks the substrate's client on the Detection facet (APMF_Sources.ini): the anchor's knowledge would not stand";
        // A client substrate opens the spell equip: its per-hand gates and the hands' stow must stand (F1 / F3).
        if (!a_sub.dryRun) return apmf::substrateequip::OpenRefusal();
        return nullptr;
    }

    void EndUnopened(Sub& a_sub, const char* a_why) {
        ++g_refusedOpen;
        spdlog::warn("[substrate] 0x{} NOT OPENED: {}.{}", Hex(a_sub.id), a_why,
                     a_sub.handle ? fmt::format(" Harbinger released the claim h={}.", a_sub.handle)
                                  : std::string(" (dry run)"));
        if (a_sub.handle && apmf::ControlMap::Get().IsClaimLive(a_sub.handle))
            apmf::ControlMap::Get().EnqueueRelease(a_sub.handle);
    }

    void FileFloors(Sub& a_sub) {
        APMF_API::APMF_CastRequest r{};
        r.flags = APMF_API::kCastFlag_DenyHandOnly;   // a FULL floor (no kCastFlag_FloorSpellsOnly)
        r.ttlMs = APMF_API::kCastMaxTtlMs;            // review F5: the longest window; Serve renews it by Repoint
        a_sub.floorR = apmf::ControlMap::Get().EnqueueCast(a_sub.id, kFloorBasis, &r);
        r.flags |= APMF_API::kCastFlag_LeftHand;
        a_sub.floorL = apmf::ControlMap::Get().EnqueueCast(a_sub.id, kFloorBasis, &r);
        if (a_sub.handle) {   // the floors rank as the substrate's own client (a normal ranked claim)
            apmf::sources::InheritClient(a_sub.floorR, a_sub.handle);
            apmf::sources::InheritClient(a_sub.floorL, a_sub.handle);
        }
    }

    void ReleaseFloors(Sub& a_sub) {
        for (const auto h : { a_sub.floorR, a_sub.floorL }) {
            if (!h) continue;
            apmf::ControlMap::Get().EnqueueRelease(h);
            // The InheritClient mapping is NOT forgotten here: the floor stays published until the release's
            // Drain, and a hand-block decision in that window would read an unmapped floor as "an internal claim"
            // (priority 50, a tie: the 12 "internal claim" STANDS lines). Sources::Pump purges the mapping once
            // the claim is no longer live (60 s after issue). DELIBERATE CHANGE (2026-10-18 review): the floor
            // now ranks as its owner client for its whole life, the release-to-Drain window included. That is an
            // arbitration change only when the owner's Equip priority differs from the default 50 (it used to
            // rank at the client default in that window).
        }
        a_sub.floorR = a_sub.floorL = APMF_API::kInvalidHandle;
    }

    // kRequested -> kFloorsQueued. False = the substrate does not exist (caller erases).
    bool Begin(Sub& a_sub) {
        auto* actor  = RE::TESForm::LookupByID<RE::Actor>(a_sub.id);
        auto* anchor = Anchor();
        auto* proc   = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
        const char* why = nullptr;
        if (!actor)                                  why = "the actor does not resolve";
        else if (actor->IsPlayerRef())               why = "the actor is the player";
        else if (!actor->Is3DLoaded())               why = "the actor is not loaded";
        else if (actor->IsDead())                    why = "the actor is dead";
        else if (actor->IsDisabled())                why = "the actor is disabled";
        else if (!proc)                              why = "the actor has no AI process";
        else if (!proc->InHighProcess())             why = "the actor is not high process";
        else if (HasController(actor))               why = "the actor is IN COMBAT (the substrate is for an out-of-combat cast; a real fight casts through ch.8b as it is)";
        else if (actor->GetCurrentScene())           why = "the actor is in a scene";
        else if (PlayerInCombat())                   why = "the player is in combat (real combat: no substrate)";
        else if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME))
                                                     why = "the dialogue menu is open";
        else if (!anchor)                            why = "the anchor actor (APMF.esl 0x80A) does not resolve";
        else if (anchor->IsDead())                   why = "the anchor is dead (it is essential and a ghost; something killed it)";
        if (!why) why = RankRefusal(a_sub);
        if (why) {
            EndUnopened(a_sub, why);
            return false;
        }

        std::uint32_t st = kStLive | kStRankAction | kStRankCombat | kStRankDetect;
        apmf::CastSeatClaim c{};
        if (DrivingClaim(a_sub.id, c)) st |= kStCast;
        const int slot = TakeSlot(a_sub.id, actor->GetHandle().native_handle(), st);
        if (slot < 0) {
            EndUnopened(a_sub, "all 16 substrate slots are in use");
            return false;
        }
        a_sub.slot = slot;
        apmf::substrateequip::Begin(slot, a_sub.id, apmf::clock::MonotonicMs());   // the equip window
        if (!a_sub.dryRun) {   // weapon / shield / torch off both hands (F1); kStEquipOk waits for StowSettled (Enter)
            apmf::substrateequip::Stow(a_sub.id, actor);
            a_sub.stowAsked = true;
        }
        if (g_anchorOut++ == 0) {   // 0 -> 1: the anchor comes out of its holding cell
            g_anchorH.store(anchor->GetHandle().native_handle(), std::memory_order_release);
            AnchorToHolder(anchor, actor, true);
            g_served = a_sub.id;
        }
        FileFloors(a_sub);
        a_sub.phase    = Phase::kFloorsQueued;
        a_sub.queuedMs = apmf::clock::MonotonicMs();
        return true;
    }

    void CloseSub(Sub& a_sub, const char* a_reason);

    std::string StaysText(RE::FormID a_stays) {
        return a_stays ? fmt::format("stays for 0x{}", Hex(a_stays))
                       : std::string("stays out (no open holder yet: the next one to enter takes it)");
    }

    // kFloorsQueued -> kOpen: THE ENTRY. False = closed (caller erases).
    bool Enter(Sub& a_sub) {
        auto& cm  = apmf::ControlMap::Get();
        const auto now = apmf::clock::MonotonicMs();
        if (!(cm.IsClaimLive(a_sub.floorR) && cm.IsClaimLive(a_sub.floorL))) {
            if (now - a_sub.queuedMs < kFloorWaitMs) return true;   // wait for the Drain that publishes them
            spdlog::warn("[substrate] 0x{} OPEN anchor=0x{} entry=false -- the hand floors were not published within "
                         "{} ms (a floor was refused: see the [ch.8b] lines above); no entry was made.",
                         Hex(a_sub.id), Hex(g_anchorId.load()), kFloorWaitMs);
            CloseSub(a_sub, "floors not published");
            return false;
        }
        auto* actor  = RE::TESForm::LookupByID<RE::Actor>(a_sub.id);
        auto* anchor = Anchor();
        auto* proc   = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
        const char* why = nullptr;
        if (!actor || !actor->Is3DLoaded() || actor->IsDead() || !proc) why = "the actor is gone, unloaded or dead";
        else if (HasController(actor))                               why = "the actor entered combat before the substrate did";
        else if (!anchor)                                            why = "the anchor does not resolve";
        else if (g_slots[a_sub.slot].state.load() & kStReal)         why = "real combat started before the entry";
        else if (PlayerInCombat())                                   why = "the player entered combat before the entry";
        if (why) {
            spdlog::warn("[substrate] 0x{} OPEN anchor=0x{} entry=false -- {}; no entry was made.", Hex(a_sub.id),
                         Hex(g_anchorId.load()), why);
            CloseSub(a_sub, why);
            return false;
        }
        // The hands' stow (review F1 of a0112b24) must have applied BEFORE StartCombat (its re-arm equip runs inside it).
        {
            const char* stowWhy = nullptr;
            const auto  stow    = apmf::substrateequip::StowSettled(a_sub.id, actor, now, stowWhy);
            if (stow == apmf::substrateequip::StowState::kWaiting) return true;
            if (stow == apmf::substrateequip::StowState::kFailed) {
                spdlog::warn("[substrate] 0x{} OPEN anchor=0x{} entry=false -- {}; no entry was made.", Hex(a_sub.id),
                             Hex(g_anchorId.load()), stowWhy ? stowWhy : "the hands were not stowed");
                CloseSub(a_sub, "hands not stowed");
                return false;
            }
            // The spell equip opens ONLY here for a client Open (hands clear, checks passed at Begin) -- never for a dry
            // run; a dry run taken over later gets it in Serve (EquipTakeover).
            if (a_sub.stowAsked && !a_sub.dryRun) {
                a_sub.equipOk = true;
                g_slots[a_sub.slot].state.fetch_or(kStEquipOk, std::memory_order_acq_rel);
            }
        }
        // StartCombat's own distance test (AE 0x6B6A30-0x6B6A6B) measures the holder to its target: the
        // anchor stands at the entering holder for the call; PlaceAnchor re-places it after.
        if (g_served != a_sub.id) {
            AnchorToHolder(anchor, actor, false);
            g_served = a_sub.id;
        }
        a_sub.entryBit = proc->ignoringCombat;
        apmf::substrateprobe::BeforeEntry(a_sub.id, proc, now);   // R1: the aggressor bit before the entry
        apmf::substratepackage::Opening(a_sub.id, proc);   // F-6: the pre-substrate package
        bool ok = false;
        // R5 F-2: the bark deny's record is PENDING before the call (the holder's UpdateCombat job is not ordered
        // against this thread): its next UpdateCombat pre-arms the group's two timers before the engine's update.
        // fix/apmf-ooc-substrate: the bark is requested on the controller's FIRST update, which can come as late as the
        // tree start (up to kTreeStartWaitMs), and the TTL now runs from there: the timers cover both, plus 2 s.
        apmf::substratebark::Arm(a_sub.slot, a_sub.id, actor->GetHandle().native_handle(),
                                 static_cast<float>(kTreeStartWaitMs + a_sub.ttlMs) / 1000.0f + 2.0f);
        g_slots[a_sub.slot].state.fetch_or(kStEntering, std::memory_order_acq_rel);
        {
            // The call ch.21 makes (verified row CombatEntry.Actor.StartCombat); a declared entry passes a
            // ch.22 re-entry deny on the holder (INVARIANTS #0 (h) condition 4).
            const apmf::reentrydeny::ClientEntryScope pass(a_sub.id);
            ok = actor->StartCombat(anchor, nullptr);
        }
        g_slots[a_sub.slot].state.fetch_and(~kStEntering, std::memory_order_acq_rel);
        const bool cc = HasController(actor);
        if (!ok || !cc) {
            ++g_entryRefused;
            spdlog::warn("[substrate] 0x{} OPEN anchor=0x{} entry=false -- StartCombat returned {}{}.", Hex(a_sub.id),
                         Hex(g_anchorId.load()), ok ? "true" : "false",
                         ok ? " but left NO controller" : " (the engine refused: its life-state, dead, distance or flag tests)");
            apmf::substratebark::Disarm(a_sub.slot);
            CloseSub(a_sub, "entry refused");
            return false;
        }
        apmf::substrateaggressor::AfterEntry(a_sub.id, proc);   // R5 F-3: StartCombat marked him the aggressor
        // R2 research F-1: the bit is off at entry, so StartCombat's block at AE 0x6B6CFB-0x6B6D32 ran. It is NOT a
        // topic: the scene notification (vfunc 0x4A into 24131; the holder is in no scene) and a head-track clear
        // (39472, slot 5). The combat-start BARK is the controller's first update (33231): channels/SubstrateBark.cpp.
        if (!a_sub.entryBit)
            spdlog::info("[substrate] 0x{} StartCombat's in-combat block ran (the ignoring-combat bit was off at entry; AE "
                         "0x6B6CFB: a scene notification and a head-track clear, NOT a dialogue line). The combat-start "
                         "bark comes from the controller's first update: {}.",
                         Hex(a_sub.id),
                         apmf::substratebark::On() ? std::string("the bark deny pre-arms it (the BARK DENY line at the close)")
                                                   : fmt::format("NOT denied, the bark deny is OFF ({})",
                                                                 apmf::substratebark::OffReason()));
        else
            spdlog::warn("[substrate] 0x{} the ignoring-combat bit was ALREADY ON at entry (the running package carries "
                         "IgnoreCombat): StartCombat's in-combat block was skipped, but the new controller copied the bit into its +0x42 "
                         "at birth (AE 0x5582F8) and is born PAUSED -- its tree, the cast leaves included, does not run. "
                         "P6's ctrl+0x42 shows it.",
                         Hex(a_sub.id));
        // S2a, review F2 of a512763: the package RE-PICK FIRST, then the process bit ONCE after it ran (see
        // WriteBitAfterRepick). The controller is left unpaused. No re-assert (S2b).
        g_evaluatePackage(actor, false, false);   // resetAI false: a reset clears the combat group (and StopCombats)
        a_sub.bitPending    = true;
        a_sub.repickAskedMs = now;
        a_sub.floorRenewMs  = now;
        spdlog::info("[substrate] 0x{} package re-pick requested (EvaluatePackage(false, false): buffered, kEvpBuffered "
                     "{}); the ignoring-combat bit is written once the engine ran it. Controller +0x42 = {} (not paused).",
                     Hex(a_sub.id),
                     actor->GetActorRuntimeData().boolBits.all(RE::Actor::BOOL_BITS::kEvpBuffered) ? "set" : "NOT set",
                     actor->GetActorRuntimeData().combatController->ignoringCombat ? 1 : 0);
        a_sub.phase      = Phase::kOpen;
        a_sub.openedMs   = now;
        // fix/apmf-ooc-substrate: until the tree first runs the substrate waits the start FLOOR; Serve moves the
        // deadline to (tree start + TTL) the frame it sees the start.
        a_sub.deadlineMs = now + kTreeStartWaitMs;
        a_sub.lastPos    = actor->GetPosition();
        a_sub.lastPosMs  = now;
        a_sub.musicAtOpen = g_musicOk ? MusicNow() : 0;
        ++g_opened;
        apmf::substrateprobe::Opened(a_sub.id, now, anchor);
        if (!g_musicDeny)
            spdlog::warn("[substrate] 0x{} known hole: the music deny is OFF ({}): the engine may decide this group's "
                         "music and battle music can play while the substrate is open.",
                         Hex(a_sub.id), g_musicDenyOff);
        if (!g_moveHold.load(std::memory_order_relaxed))
            spdlog::warn("[substrate] 0x{} known hole: the movement hold is OFF ({}): the Movement half's leaves are "
                         "force-failed, which fails the Combat Parallel and kills its Action half -- no cast is reachable.",
                         Hex(a_sub.id), g_moveHoldOff);
        auto*       aproc = anchor->GetActorRuntimeData().currentProcess;
        const char* alvl  = !aproc ? "none (no AI process)" : aproc->InHighProcess() ? "HIGH (SetAlpha applies)"
                                                                               : "not high (SetAlpha is a no-op until it is)";
        spdlog::info("[substrate] 0x{} OPEN anchor=0x{} entry=true ({}; holders {}; TTL {:.1f} s; floors h={}/h={}; "
                     "fire signal {}; anchor process {}, 3D {}){}",
                     Hex(a_sub.id), Hex(g_anchorId.load()), a_sub.dryRun ? "DRY RUN: no claim, no cast" :
                                                             fmt::format("claim h={}", a_sub.handle),
                     LiveHolders(), static_cast<double>(a_sub.ttlMs) / 1000.0, a_sub.floorR, a_sub.floorL,
                     g_fireSignal ? "the census 0x0B seat" : "NONE (the census 0x0B seat is not installed: a fire cannot close the substrate)",
                     alvl, anchor->Is3DLoaded() ? "loaded" : "not loaded",
                     a_sub.entryBit ? " -- NOTE: the bit was already ON (the controller was born paused)" : "");
        return true;
    }

    // ---- close / drop ------------------------------------------------------------------------
    void ReleaseClientClaim(Sub& a_sub, const char* a_why) {
        if (!a_sub.handle) return;
        if (!apmf::ControlMap::Get().IsClaimLive(a_sub.handle)) return;
        apmf::ControlMap::Get().EnqueueRelease(a_sub.handle);
        spdlog::info("[substrate] 0x{} claim h={} ended by Harbinger: {}.", Hex(a_sub.id), a_sub.handle, a_why);
    }

    // a_stays: the holder the anchor still stands at when it does not go home. When it stood at THIS holder,
    // g_served is cleared for the placement pass (it re-places the anchor at the best remaining holder this
    // frame), so a_stays names the holder that pass will pick (PlaceAnchor's rule: an open holder, the shortest-range
    // claim, the earliest open on a tie) instead: the log reads "stays for 0x...", never
    // "stays for 0x00000000" (ClickUp 86e3pu939: the old code read g_served after clearing it; a log bug only).
    void LeaveSlotAndAnchor(Sub& a_sub, bool& a_home, RE::FormID& a_stays) {
        ClearSlot(a_sub.slot);
        a_sub.slot = -1;
        if (g_anchorOut > 0) --g_anchorOut;
        a_home  = false;
        a_stays = 0;
        if (g_anchorOut == 0) {
            AnchorHome(Anchor(), "last holder");
            a_home = true;
            return;
        }
        if (g_served == a_sub.id) g_served = 0;   // the placement pass picks the next holder this frame
        a_stays = g_served;
        if (a_stays == 0) {
            const Sub* next = nullptr;
            for (const auto& [id, o] : g_subs)
                if (id != a_sub.id && o.slot >= 0 && o.phase == Phase::kOpen &&
                    (!next || o.tier < next->tier || (o.tier == next->tier && o.openedMs < next->openedMs)))
                    next = &o;
            if (next) a_stays = next->id;
        }
    }

    // fix/apmf-ooc-substrate: the window's proof line (TTL used against the measured start; the movement hold; the
    // Combat Parallel's entries). Read before the slot is cleared.
    std::string WindowText(const Sub& a_sub, const Slot* a_s, std::uint64_t a_now) {
        if (!a_s) return "no slot";
        const std::uint32_t cp = a_s->combatParallelActs.load(std::memory_order_relaxed);
        const std::uint32_t h = a_s->holds.load(std::memory_order_relaxed), hu = a_s->holdUpdates.load(std::memory_order_relaxed),
                            hr = a_s->holdReleased.load(std::memory_order_relaxed),
                            ha = a_s->holdAnomalies.load(std::memory_order_relaxed);
        const std::string tree =
            a_sub.treeStarted
                ? fmt::format("tree started +{} ms after the open, TTL {} ms ran from there (used {} ms)",
                              a_sub.treeStartAt > a_sub.openedMs ? a_sub.treeStartAt - a_sub.openedMs : 0, a_sub.ttlMs,
                              a_now > a_sub.treeStartAt ? a_now - a_sub.treeStartAt : 0)
                : fmt::format("the tree never ran ({} ms open; start floor {} ms)", a_now - a_sub.openedMs, kTreeStartWaitMs);
        const std::string hold =
            !g_moveHold.load(std::memory_order_relaxed)
                ? fmt::format("movement hold OFF ({})", g_moveHoldOff)
                : fmt::format("movement half held {} time(s), {} held update(s), released {}{}", h, hu, hr,
                              ha ? fmt::format(", ANOMALIES {}", ha) : std::string());
        // fix/apmf-substrate-release-close (APMF-B93): the release, as the window saw it
        const std::uint64_t evMs = a_s->castEventMs.load(std::memory_order_relaxed);
        const std::string   ev   = evMs ? fmt::format("TESSpellCastEvent 0x{} at +{} ms after the open",
                                                      Hex(a_s->castEventForm.load(std::memory_order_relaxed)),
                                                      evMs > a_sub.openedMs ? evMs - a_sub.openedMs : 0)
                                        : std::string("no TESSpellCastEvent for the driven form");
        const std::string rel =
            a_sub.releasing
                ? fmt::format("release: NotifyStartCast 0x{} at +{} ms after the open, waited {} ms (bound {} ms), hand max "
                              "state {} while releasing, closed by {}; {}",
                              Hex(a_sub.releaseForm), a_sub.releaseAtMs > a_sub.openedMs ? a_sub.releaseAtMs - a_sub.openedMs : 0,
                              a_now > a_sub.releaseAtMs ? a_now - a_sub.releaseAtMs : 0, kReleaseWaitMs, a_sub.releaseHandMax,
                              a_sub.releaseHow ? a_sub.releaseHow : "NOTHING (not released)", ev)
                : fmt::format("release: no NotifyStartCast for the driven form{}; {}",
                              a_sub.releaseHow ? fmt::format(" (closed by {})", a_sub.releaseHow) : std::string(), ev);
        return fmt::format("{} | {} | Combat Parallel entered {} time(s) (1 = it kept running; the fail loop re-entered "
                           "it every frame) | {}",
                           tree, hold, cp, rel);
    }

    // fix/apmf-substrate-release-close (deny-hole probe, window 2 of 2026-10-26): ForLeaf's early returns for this
    // holder, by the slot bit that failed, and the first one's leaf and node. Read before the slot is cleared.
    std::string GateText(const Sub& a_sub, const Slot* a_s) {
        if (!a_s) return "no slot";
        const std::uint32_t nl = a_s->gateNotLive.load(std::memory_order_relaxed), re = a_s->gateReal.load(std::memory_order_relaxed),
                            nr = a_s->gateNoRank.load(std::memory_order_relaxed), bi = a_s->gateBadIdx.load(std::memory_order_relaxed);
        const std::uint32_t total = nl + re + nr + bi;
        const std::uint64_t ts    = a_s->treeStartMs.load(std::memory_order_relaxed);
        const std::string   tree  = ts ? fmt::format("the gate saw the tree from +{} ms after the open",
                                                    ts > a_sub.openedMs ? ts - a_sub.openedMs : 0)
                                       : std::string("the gate NEVER saw this holder's tree (treeStartMs 0)");
        const RE::FormID    bo    = a_s->builtOutside.load(std::memory_order_relaxed);
        const std::uint64_t boMs  = a_s->builtOutsideMs.load(std::memory_order_relaxed);
        const std::string   built = bo ? fmt::format("; a caster for 0x{} was BUILT outside the gate at +{} ms after the open",
                                                     Hex(bo), boMs > a_sub.openedMs ? boMs - a_sub.openedMs : 0)
                                       : std::string();
        if (total == 0)
            return fmt::format("ForLeaf early returns 0 ({}{}). 0 with a tree the gate never saw = the holder's leaves never "
                               "reached ForLeaf (the act seat's deliberating-actor resolve or its leaf table), not a slot bit",
                               tree, built);
        const std::uint32_t li   = a_s->gateFirstLeaf.load(std::memory_order_relaxed);
        const void*         node = a_s->gateFirstNode.load(std::memory_order_relaxed);
        // the node's name: the BSFixedString at node+0x08 (see NodeNamed: a static tree node alive for the session)
        const char* nn = node ? reinterpret_cast<const RE::BSFixedString*>(static_cast<const std::uint8_t*>(node) + 0x08)->c_str()
                              : nullptr;
        const std::uint64_t fm = a_s->gateFirstMs.load(std::memory_order_relaxed);
        return fmt::format("ForLeaf early returns {} (kStLive clear {}, kStReal set {}, kStRankAction clear {}, leaf index out "
                           "of range {}); the first at +{} ms after the open: {} on leaf '{}' node {} '{}' ({}{})",
                           total, nl, re, nr, bi, fm > a_sub.openedMs ? fm - a_sub.openedMs : 0,
                           GateWhyName(a_s->gateFirstWhy.load(std::memory_order_relaxed)),
                           li < subs::kLeafCount && g_leafName[li] ? g_leafName[li] : "?", fmt::ptr(node), nn ? nn : "?", tree, built);
    }

    void CloseSub(Sub& a_sub, const char* a_reason) {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_sub.id);
        auto* proc  = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
        apmf::substrateprobe::Closing(a_sub.id, actor, a_sub.phase == Phase::kOpen, false, apmf::clock::MonotonicMs());   // R1
        apmf::substratepackage::Closing(a_sub.id, actor, false);   // F-6 summary
        apmf::substrateequip::Closing(a_sub.slot, a_sub.id, actor, a_reason, apmf::clock::MonotonicMs());   // equip outcome
        apmf::substrateequip::RestoreWhenFree(a_sub.id, a_reason, apmf::clock::MonotonicMs());   // the stowed hands (F2)
        if (a_sub.slot >= 0 && a_sub.phase == Phase::kOpen) LogObserved(a_sub, actor);
        if (a_sub.phase == Phase::kOpen) apmf::substratebark::Closing(a_sub.slot, a_sub.id, false, apmf::clock::MonotonicMs());
        else apmf::substratebark::Disarm(a_sub.slot);   // R5 F-2 (a close before the entry: no line)
        if (const auto t = apmf::substrateaggressor::Closing(a_sub.id); !t.empty()) spdlog::info("[substrate] 0x{} {}.", Hex(a_sub.id), t);
        DebtForget(a_sub.id);   // a normal close: his StopCombat ends his part in the group (a co-holder keeps its own debt)
        // 1. the bit back to what it was
        if (a_sub.bitSet && proc) proc->ignoringCombat = a_sub.priorBit;
        // 2. StopCombat once (the substrate entered, so the controller is the substrate's). Its group's other
        //    members are read first: a non-holder among them is cleaned after the StopCombat (anchor guard).
        const auto members = a_sub.phase == Phase::kOpen ? apmf::anchorguard::GroupMembers(actor)
                                                         : std::vector<RE::ActorHandle>{};
        bool stopped = false;
        if (a_sub.phase == Phase::kOpen && actor && HasController(actor)) {
            if (*reinterpret_cast<const std::uintptr_t*>(actor) == g_vtChar) {
                actor->StopCombat();   // Character slot 0xE5 (AE 0x6B70A0 / SE 0x625920)
                stopped = true;
            } else {
                spdlog::error("[substrate] 0x{} close: NOT a Character vtable -- StopCombat not called.", Hex(a_sub.id));
            }
        }
        const bool ccNull = !HasController(actor);
        apmf::anchorguard::AfterClose(a_sub.id, members);   // every non-holder still in that group (SEV-1 2026-10-19)
        // 3. the substrate's internal effects
        ReleaseFloors(a_sub);
        Slot* s = a_sub.slot >= 0 ? &g_slots[a_sub.slot] : nullptr;
        const std::string leaves = s ? LeafCounts(*s) : std::string("none");
        const std::uint32_t denied = s ? s->leafDenied.load() : 0, moved = s ? s->moveDenied.load() : 0,
                            casts  = s ? s->castAllowed.load() : 0;
        const std::string window = a_sub.phase == Phase::kOpen ? WindowText(a_sub, s, apmf::clock::MonotonicMs()) : std::string();
        const std::string gate   = a_sub.phase == Phase::kOpen ? GateText(a_sub, s) : std::string();
        // fix/apmf-ooc-substrate: no controller left = its behaviour threads are freed (StopCombat frees them without a
        // pop): the movement hold's records of this holder are forgotten. A live controller keeps them (each ends with
        // its node's own pop, after the update seat's failure exit).
        if (ccNull) apmf::actiongate::ForgetMovementHolds(a_sub.id);
        // 4. out of the holder set; the anchor home on 1 -> 0
        bool       home = false;
        RE::FormID stays = 0;
        if (a_sub.slot >= 0) LeaveSlotAndAnchor(a_sub, home, stays);
        // 5. one package re-pick
        if (actor && a_sub.phase == Phase::kOpen) g_evaluatePackage(actor, false, false);
        ++g_closed;
        if (a_sub.phase == Phase::kOpen)
            spdlog::info("[substrate] 0x{} CLOSE reason={} controller-null={} anchor-home={} pkg-after=0x{} (StopCombat {}, "
                         "open {} ms, leaves denied {} [movement {}] {}, cast leaves let through {}, ignoring-bit resets {})",
                         Hex(a_sub.id), a_reason, ccNull ? "y" : "n", home ? "y" : fmt::format("n ({})", StaysText(stays)),
                         Hex(PackageOf(actor)), stopped ? "called" : "not needed", apmf::clock::MonotonicMs() - a_sub.openedMs,
                         denied, moved, leaves, casts, a_sub.bitResets);
        if (a_sub.phase == Phase::kOpen)
            spdlog::info("[substrate] 0x{} WINDOW ({}): {}", Hex(a_sub.id), a_reason, window);
        // fix/apmf-substrate-release-close: one LEAF GATE line per window (bounded by the open rate), at the close
        if (a_sub.phase == Phase::kOpen)
            spdlog::info("[substrate] 0x{} LEAF GATE ({}): {}", Hex(a_sub.id), a_reason, gate);
        if (a_sub.phase != Phase::kOpen)
            spdlog::info("[substrate] 0x{} CLOSE reason={} (before the entry) anchor-home={}", Hex(a_sub.id), a_reason,
                         home ? "y" : "n");
        ReleaseClientClaim(a_sub, a_reason);
    }

    // design 3.6a: the game-thread pass after the stand-down. NO StopCombat while a real foe is in the group;
    // a fight left with no target is ended, ranked (marth 2026-10-13, F6).
    void DropSub(Sub& a_sub) {
        Slot&         s     = g_slots[a_sub.slot];
        const auto    why   = s.realWhy.load(std::memory_order_acquire);
        const RE::FormID foe = s.realFoe.load(std::memory_order_relaxed);
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_sub.id);
        auto* proc  = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
        apmf::substrateprobe::Closing(a_sub.id, actor, true, true, apmf::clock::MonotonicMs());   // R1
        const std::uint32_t pkgDenied = apmf::substratepackage::Closing(a_sub.id, actor, true);   // F-6 summary
        apmf::substratebark::Closing(a_sub.slot, a_sub.id, true, apmf::clock::MonotonicMs());   // R5 F-2: line; owed put-back
        if (const auto t = apmf::substrateaggressor::Closing(a_sub.id); !t.empty()) spdlog::info("[substrate] 0x{} {}.", Hex(a_sub.id), t);
        apmf::substrateequip::Closing(a_sub.slot, a_sub.id, actor, "real combat (dropped)", apmf::clock::MonotonicMs());
        apmf::substrateequip::RestoreWhenFree(a_sub.id, "real combat (dropped)", apmf::clock::MonotonicMs());   // fights with his gear
        if (a_sub.bitSet && proc) proc->ignoringCombat = a_sub.priorBit;   // the package no longer runs in combat
        if (pkgDenied && actor) g_evaluatePackage(actor, false, false);   // F-6 review: the real fight gets its combat-override pick (REAL: the seat answers the engine's)
        // The music debt (review F1): an (a)-trigger restore already ran in the seat that saw the real target. Any 0 still
        // owed (a hit / party trigger, or a merge into an ally's fight) is put back by the seat on the next detection pass
        // that sees a real target in a group holding this holder, for kDebtAfterDropMs.
        const std::uint32_t mu = s.music.load(std::memory_order_acquire);
        DebtTimeout(a_sub.id, apmf::clock::MonotonicMs() + kDebtAfterDropMs);
        const std::string musicNote =
            !(mu & kMusWrote) ? std::string()
            : (mu & kMusRestored) ? std::string("; music: Harbinger's 0 already put back to 0xFF")
                                  : fmt::format("; music: Harbinger's 0 is owed back for {} s (the seat puts it back on a "
                                                "real target in any group holding him)", kDebtAfterDropMs / 1000);
        // the anchor out of the holder's group targets (the engine's own RemoveTarget)
        std::string removed = "no controller";
        if (auto* cc = actor ? actor->GetActorRuntimeData().combatController : nullptr) {
            if (auto* g = cc->combatGroup; g && Anchor()) {
                g_removeTarget(g, Anchor());
                std::uint32_t left = 0;
                bool          still = false;
                {
                    RE::BSReadLockGuard guard(g->lock);
                    left = g->targets.size();
                    still = FindTargetEntry(g, g_anchorH.load()) != nullptr;
                }
                removed = fmt::format("anchor removed from the group targets (RemoveTarget){}; {} target(s) left",
                                      still ? " -- STILL LISTED" : "", left);
                // marth 2026-10-13 (review F6, "End an empty fight"): a trigger that is not in the holder's group
                // ((b) a hit, (c) the party) leaves a fight with NO target once the anchor is gone. That one is ended
                // through the held engagement's RANKED StopCombat; a fight with the real foe in it never is.
                if (left == 0) {
                    auto*             anc  = Anchor();
                    const std::string stop = apmf::combatentry::hold::StopCombatRanked(
                        actor, a_sub.handle, anc ? anc->GetFormID() : 0, anc ? anc->GetHandle() : RE::ActorHandle{});
                    removed += fmt::format("; EMPTY FIGHT: {}", stop);
                    // round-2 closing item 3a: the same package re-pick CloseSub makes after its StopCombat.
                    if (stop.starts_with("combat ENDED")) {
                        g_evaluatePackage(actor, false, false);
                        removed += " + package re-pick";
                    }
                } else {
                    removed += "; the real foe is in the group: NO StopCombat (the fight continues)";
                }
            } else {
                removed = "no combat group";
            }
        }
        apmf::CastSeatClaim c{};
        APMF_API::APMF_Param cp{};
        float                cb = 0.0f;
        APMF_API::Handle     ch = APMF_API::kInvalidHandle;
        const bool driving = DrivingClaim(a_sub.id, c) &&
                             apmf::ControlMap::Get().TryGetOwningClaimBasis(a_sub.id, APMF_API::kIntent_Cast, cp, cb, &ch);
        ReleaseFloors(a_sub);
        const std::uint32_t before = LiveHolders() + 1;
        bool       home  = false;
        RE::FormID stays = 0;
        const std::uint32_t afterReal = s.afterReal.load();
        const std::string   window    = WindowText(a_sub, &s, apmf::clock::MonotonicMs());
        LeaveSlotAndAnchor(a_sub, home, stays);
        ++g_drops;
        // fix/apmf-ooc-substrate: the fight goes on with this controller, so a held Movement node is NOT forgotten here:
        // its next update is no longer held and ends it through its own failure exit (the tree restarts once, as the
        // engine built it), and its own pop removes the record.
        spdlog::info("[substrate] 0x{} WINDOW (real combat): {}", Hex(a_sub.id), window);
        spdlog::info("[substrate] 0x{} REAL COMBAT ({} 0x{}) -- substrate DROPPED, combat behaviour restored in the same "
                     "frame: {}, holders {}->{}, anchor {}; released: leaf allow-list, movement denies, ignore-combat hold, "
                     "anchor refusal, substrate floors; claim {}",
                     Hex(a_sub.id), kWhyName[why < 4 ? why : 0], Hex(foe), removed + musicNote, before, before - 1,
                     home ? std::string("home") : StaysText(stays),
                     driving ? fmt::format("h={} kept", ch) : std::string("none (no driving cast claim)"));
        if (afterReal)
            spdlog::warn("[substrate] 0x{} {} leaf deny(s) fired AFTER the real-combat trigger (see the DENIED ... AFTER "
                         "REAL COMBAT lines): a defect, not a fallback.",
                         Hex(a_sub.id), afterReal);
        ReleaseClientClaim(a_sub, "real combat (the substrate dropped; the fight goes on)");
    }

    // ---- the single bit write (review F2 of a512763) ---------------------------------------------
    // EvaluatePackage(false, false) (AE 0x66CCF0 / SE 0x5DB310) does not evaluate: its deferred branch sets
    // boolBits kEvpBuffered (bit 16; AE 0x66D13C `bts eax,0x10` / SE 0x5DB732 `or [rdi+0xE0],0x10000`) and a
    // process timer (+0x48). The actor's own AI update (37347, AE 0x6672B0) consumes it: at AE 0x6677E6 it
    // tests bit 16 and calls EvaluatePackage(immediate) (0x667804; SE 36356 0x5D6840 at 0x5D6B39), whose
    // immediate path clears bits 16-17 (AE 0x66D117 `and [rdi+0xE8],0xFFFCFFFF` / SE 0x5DB713 on +0xE0); later in the SAME update (AE 0x667ACC -> 39108 -> 39213 ->
    // 39434 0x6EA820) the package start runs UpdateIgnoringCombat (AE 0x6D7E80, called at 0x6EA9A3), which
    // copies the running package's IgnoreCombat flag into +0x139 -- 0 for a follow package. Writing the bit
    // before that update (as S2a first did) let Harbinger's own re-pick undo it. So: request the re-pick,
    // wait until kEvpBuffered reads CLEAR (the update ran its evaluation), then wait ONE MORE Poll (the
    // same update's package start has finished by then, whichever thread ran it), then write the bit once.
    // If the re-pick never runs in kRepickWaitMs the bit is written anyway and that is logged.
    // R5 F-5 (R2 research Q7: both holders stalled in the same two windows -> a global cause; the actor's AI update, the
    // only consumer of kEvpBuffered, does not run while a menu pauses the game): the wait counts only the time between two
    // Polls in which the game was not paused (UI::GameIsPaused, numPausesGame > 0) and the gap was a frame (<= kPollGapMs).
    // A longer gap is "stalled or paused (menu)": Poll rides the player's update, which does not run while a pausing menu
    // is open, so such a pause shows up as one long gap, not as paused Polls (review F8 of e6f5607c: no precise split).
    void WriteBitAfterRepick(Sub& a_sub, RE::Actor* a_actor, RE::AIProcess* a_proc, std::uint64_t a_now) {
        const bool buffered = a_actor->GetActorRuntimeData().boolBits.all(RE::Actor::BOOL_BITS::kEvpBuffered);
        {
            const std::uint64_t last = a_sub.repickLastPollMs ? a_sub.repickLastPollMs : a_sub.repickAskedMs;
            const std::uint64_t dt   = a_now > last ? a_now - last : 0;
            auto*               ui   = RE::UI::GetSingleton();
            if (ui && ui->GameIsPaused()) a_sub.repickPausedMs += dt;
            else if (dt > kPollGapMs)     a_sub.repickGapMs += dt;
            else                          a_sub.repickRunMs += dt;
            a_sub.repickLastPollMs = a_now;
        }
        const bool timedOut = a_sub.repickRunMs >= kRepickWaitMs;
        if (buffered && !timedOut) return;
        if (!buffered && !a_sub.repickClearFrame) {
            a_sub.repickClearFrame = g_pollFrame;
            a_sub.repickClearMs    = a_now;
            return;   // one more Poll: the package start in that same update has run
        }
        if (!timedOut && g_pollFrame <= a_sub.repickClearFrame) return;
        if (buffered)   // R1 + R5 F-5 / F-7: the pause / menu state at the stall
            apmf::substrateprobe::RepickNotRun(a_actor, a_proc, a_now - a_sub.repickAskedMs, a_sub.repickRunMs,
                                               a_sub.repickPausedMs, a_sub.repickGapMs);
        a_sub.bitPending  = false;
        a_sub.priorBit    = a_proc->ignoringCombat;
        a_proc->ignoringCombat = true;   // AIProcess+0x139 (static_assert below; AE 0x6D6780 / SE 0x643F00 read it)
        a_sub.bitSet      = true;
        a_sub.bitWasOn    = true;
        a_sub.bitPkg      = a_proc->currentPackage.package;
        a_sub.bitPkgStart = a_proc->currentPackage.packageStartTime;
        spdlog::info("[substrate] 0x{} wrote AIProcess+0x139 (ignoringCombat) {} -> 1 ONCE (S2a: no re-assert), {} "
                     "(package 0x{}, start time {:.2f}).",
                     Hex(a_sub.id), a_sub.priorBit ? 1 : 0,
                     buffered ? fmt::format("the re-pick did NOT run within {} ms of unpaused time (kEvpBuffered still "
                                            "set; {} ms wall, {} ms seen paused, {} ms stalled or paused (menu))",
                                            kRepickWaitMs, a_now - a_sub.repickAskedMs, a_sub.repickPausedMs,
                                            a_sub.repickGapMs)
                              : fmt::format("after the re-pick ran (kEvpBuffered cleared at +{} ms; {} ms seen paused, {} "
                                            "ms stalled or paused (menu))",
                                            a_sub.repickClearMs - a_sub.repickAskedMs, a_sub.repickPausedMs,
                                            a_sub.repickGapMs),
                     Hex(a_sub.bitPkg ? a_sub.bitPkg->GetFormID() : 0), static_cast<double>(a_sub.bitPkgStart));
    }

    // ---- the dry-run TAKEOVER (review of 1f99de2b, SEV-2) ---------------------------------------
    // A client claim declared onto an open DRY-RUN substrate only clears `dryRun` (Declare): its Begin made no client
    // checks and no stow. The spell equip (kStEquipOk) must not open on those hands. Chosen: run the client Open's own
    // steps IN PLACE -- RankRefusal (the five facets + OpenRefusal), Stow, then StowSettled -- and set kStEquipOk only
    // when the hands read clear; a refusal or a failed stow CLOSES the substrate (logged). Not "close and re-open as a
    // client": that would StopCombat and StartCombat again (a second combat-start topic, a second package re-pick and
    // music decision) for the same holder within a frame, while the floors and the anchor are already in place; the
    // only thing the dry run lacked is exactly these checks and the stow, and the floors already hold both hands (the
    // sink's step 1a keeps the stow, as at a client Open). Until kStEquipOk is set the cast leaves may run (kStCast) but
    // the spell equip may not, so nothing is drawn. Returns a close reason, or nullptr (keep serving).
    const char* EquipTakeover(Sub& a_sub, RE::Actor* a_actor, std::uint64_t a_now) {
        if (a_sub.dryRun || a_sub.equipOk || !apmf::substrateequip::Enabled()) return nullptr;
        if (a_sub.handle == APMF_API::kInvalidHandle) return nullptr;   // the winning handle is read after the Drain (Poll)
        if (!a_sub.stowAsked) {
            if (const char* r = RankRefusal(a_sub)) {
                spdlog::warn("[substrate] 0x{} TAKEOVER of a dry-run substrate refused: {}.", Hex(a_sub.id), r);
                return "client takeover refused (a deny would not stand)";
            }
            apmf::substrateequip::Stow(a_sub.id, a_actor);
            a_sub.stowAsked = true;
            spdlog::info("[substrate] 0x{} TAKEOVER of a dry-run substrate: client checks passed, hands stowed; the spell "
                         "equip opens once they read clear.", Hex(a_sub.id));
        }
        const char* stowWhy = nullptr;
        switch (apmf::substrateequip::StowSettled(a_sub.id, a_actor, a_now, stowWhy)) {
        case apmf::substrateequip::StowState::kWaiting:
            return nullptr;
        case apmf::substrateequip::StowState::kFailed:
            spdlog::warn("[substrate] 0x{} TAKEOVER: {}.", Hex(a_sub.id), stowWhy ? stowWhy : "the hands were not stowed");
            return "hands not stowed (takeover)";
        default:
            a_sub.equipOk = true;
            g_slots[a_sub.slot].state.fetch_or(kStEquipOk, std::memory_order_acq_rel);
            spdlog::info("[substrate] 0x{} TAKEOVER: hands clear, the spell equip is allowed.", Hex(a_sub.id));
            return nullptr;
        }
    }

    // ---- serve (kOpen) --------------------------------------------------------------------
    // True = keep, false = closed (caller erases).
    bool Serve(Sub& a_sub, std::uint64_t a_now) {
        Slot&         s  = g_slots[a_sub.slot];
        const auto    st = s.state.load(std::memory_order_acquire);
        if (st & kStReal) {
            DropSub(a_sub);
            return false;
        }
        // fix/apmf-ooc-substrate: the TTL runs from the tree's first node under the substrate (principle 9).
        if (!a_sub.treeStarted) {
            if (const std::uint64_t t = s.treeStartMs.load(std::memory_order_relaxed); t != 0) {
                a_sub.treeStarted = true;
                a_sub.treeStartAt = t;
                a_sub.deadlineMs  = t + a_sub.ttlMs;
                spdlog::info("[substrate] 0x{} TREE STARTED +{} ms after the open (the measured start latency); the TTL "
                             "({:.1f} s) runs from here, to +{} ms after the open.",
                             Hex(a_sub.id), t > a_sub.openedMs ? t - a_sub.openedMs : 0,
                             static_cast<double>(a_sub.ttlMs) / 1000.0,
                             a_sub.deadlineMs > a_sub.openedMs ? a_sub.deadlineMs - a_sub.openedMs : 0);
            }
        }
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_sub.id);
        const char* why = a_sub.closeAsked ? a_sub.closeWhy : nullptr;
        if (!why) {
            if (!actor)                       why = "owner gone";
            else if (actor->IsDead())         why = "owner dead";
            else if (!actor->Is3DLoaded())    why = "owner unloaded";
            else if (actor->IsDisabled())     why = "owner disabled";
            else if (!HasController(actor))   why = "combat ended by the engine (no controller)";
            // fix/apmf-substrate-release-close: a release in progress is never cut by the TTL (or the start floor);
            // it is bounded by kReleaseWaitMs below instead
            else if (a_now >= a_sub.deadlineMs && !a_sub.releasing)
                why = a_sub.treeStarted ? "ttl"
                                        : "the behaviour tree never ran within the start floor (10 s after the open; measured "
                                          "starts 2.7-5.1 s, 2026-10-20)";
        }
        if (!why && !a_sub.holdLogged && s.holds.load(std::memory_order_relaxed) != 0) {
            a_sub.holdLogged = true;
            spdlog::info("[substrate] 0x{} MOVEMENT HELD (+{} ms after the open): the Movement half (\"Movement Parallel\") "
                         "is RUNNING, held, not failed -- none of its leaves entered, so the Combat Parallel keeps its "
                         "Action half (the spell equip and the cast leaves).",
                         Hex(a_sub.id), a_now - a_sub.openedMs);
        }
        // fix/apmf-substrate-release-close (deny hole, window 2 of 2026-10-26): the engine BUILT a caster for this holder
        // (census 0x06 CheckStartCast YES) while the leaf gate had never seen its tree, so no facet was denied. Never let
        // it run undenied: close loudly.
        if (!why) {
            if (const RE::FormID b = s.builtOutside.load(std::memory_order_acquire); b != 0) {
                why = "tree-outside-gate";
                const std::uint64_t bm = s.builtOutsideMs.load(std::memory_order_relaxed);
                spdlog::warn("[substrate] 0x{} TREE OUTSIDE THE GATE: a caster for 0x{} was BUILT at +{} ms after the open "
                             "while the leaf gate had never seen this holder's tree (treeStartMs 0): nothing was denied. "
                             "Closing the substrate (see the LEAF GATE line).",
                             Hex(a_sub.id), Hex(b), bm > a_sub.openedMs ? bm - a_sub.openedMs : 0);
            }
        }
        // the claim the substrate serves
        apmf::CastSeatClaim c{};
        if (!why && DrivingClaim(a_sub.id, c)) {
            a_sub.sawClaim      = true;
            a_sub.driven        = c.proxy ? c.proxy : c.spell;
            a_sub.concentration = (c.flags & APMF_API::kCastFlag_Concentration) != 0;
            a_sub.tier          = TierOf(c.spell);
            if (!(st & kStCast)) s.state.fetch_or(kStCast, std::memory_order_acq_rel);
            apmf::substrateequip::Serve(a_sub.slot, actor, c.spell, c.proxy, c.flags, a_now);   // equip lines
            const RE::FormID fired = s.firedForm.exchange(0, std::memory_order_acq_rel);
            // the census-less fallback of the "tree-outside-gate" close above (no 0x06 seat = no build signal): the hand
            // charging the driven form while the gate never saw the tree
            if (!g_fireSignal && s.treeStartMs.load(std::memory_order_relaxed) == 0) {
                const std::uint32_t hs = HandStateFor(actor, c.spell, c.proxy);
                if (hs == 2 || hs == 3) {
                    why = "tree-outside-gate";
                    spdlog::warn("[substrate] 0x{} TREE OUTSIDE THE GATE: the hand is {} the driven form 0x{} at +{} ms "
                                 "after the open while the leaf gate had never seen this holder's tree (treeStartMs 0): "
                                 "nothing was denied. Closing the substrate (see the LEAF GATE line).",
                                 Hex(a_sub.id), hs == 2 ? "charging" : "holding charged", Hex(a_sub.driven),
                                 a_now - a_sub.openedMs);
                }
            }
            // fix/apmf-substrate-release-close (APMF-B93): 0x0B on the driven form is the release REQUEST (Charged ->
            // Casting). It marks the window RELEASING; "fired" needs real release evidence; the wait is bounded.
            if (!why && !a_sub.concentration) {
                if (!a_sub.releasing && fired && (fired == c.spell || fired == c.proxy)) {
                    a_sub.releasing   = true;
                    a_sub.releaseAtMs = a_now;
                    a_sub.releaseForm = fired;
                    spdlog::info("[substrate] 0x{} RELEASING 0x{} at +{} ms after the open: NotifyStartCast (Charged -> "
                                 "Casting) is the release REQUEST; the substrate stays open until the holder's "
                                 "TESSpellCastEvent (fired) or the hand back to None (released-unconfirmed) (bound {} ms; "
                                 "the TTL defers to it).",
                                 Hex(a_sub.id), Hex(fired), a_now - a_sub.openedMs, kReleaseWaitMs);
                }
                const RE::FormID ev = s.castEventForm.load(std::memory_order_acquire);
                if (ev && (ev == c.spell || ev == c.proxy)) {
                    why             = "fired";
                    a_sub.releaseHow = "the TESSpellCastEvent";
                } else if (a_sub.releasing) {
                    // the hand read keys on the form 0x0B released (the claim's spell or proxy at that moment)
                    const std::uint32_t hs = HandStateFor(actor, a_sub.releaseForm, 0);
                    a_sub.releaseHandMax = (std::max)(a_sub.releaseHandMax, hs);
                    // review F1 of 608df67: only the release states (4 Casting, 5 Released, 6 Concluding) arm the peak;
                    // 7 is unnamed, 8 / 9 are Interrupt / Interrupt-Deselect (CommonLib MagicCaster.h State at 57be9d67). The hand back
                    // to None is NOT proof of a fire (an interrupt, an equip rescore or a currentSpell change reads the
                    // same): it closes as "released-unconfirmed", a warning. Only the TESSpellCastEvent closes as "fired".
                    if (hs >= 4 && hs <= 6) a_sub.releasePeak = true;
                    if (a_sub.releasePeak && hs == 0) {
                        why              = "released-unconfirmed";
                        a_sub.releaseHow = "the hand back to None after a release state (UNCONFIRMED: no TESSpellCastEvent; "
                                           "an interrupt reads the same)";
                        spdlog::warn("[substrate] 0x{} RELEASED-UNCONFIRMED: the hand went back to None {} ms after "
                                     "NotifyStartCast on 0x{} (max state {} while releasing) with NO TESSpellCastEvent for "
                                     "the driven form. Closing; not counted as a fire (an interrupt reads the same).",
                                     Hex(a_sub.id), a_now - a_sub.releaseAtMs, Hex(a_sub.releaseForm),
                                     a_sub.releaseHandMax);
                    } else if (a_now - a_sub.releaseAtMs >= kReleaseWaitMs) {
                        why = "fired-not-released";
                        spdlog::warn("[substrate] 0x{} FIRED-NOT-RELEASED: NotifyStartCast on 0x{} {} ms ago and no release "
                                     "evidence within {} ms (no TESSpellCastEvent for the driven form; the hand {} at "
                                     "state {}, max {} while releasing). Closing; the cast did NOT complete under the "
                                     "substrate.",
                                     Hex(a_sub.id), Hex(a_sub.releaseForm), a_now - a_sub.releaseAtMs, kReleaseWaitMs,
                                     a_sub.releasePeak ? "reached a release state (4-6) and is"
                                                       : "never read a release state (4-6); it is", hs,
                                     a_sub.releaseHandMax);
                    }
                }
            }
        } else if (!why) {
            if (st & kStCast) s.state.fetch_and(~kStCast, std::memory_order_acq_rel);
            a_sub.tier = kTierNone;
            if (a_sub.sawClaim) why = "claim ended (released, expired or its stop percent)";
        }
        if (!why) why = EquipTakeover(a_sub, actor, a_now);   // a dry run a client took over (review of 1f99de2b)
        if (why) {
            CloseSub(a_sub, why);
            return false;
        }

        // review F5: the floors live as long as the substrate. A floor that is no longer live leaves a hand
        // undenied: the substrate closes (principle 7, never a silent gap); a live one is renewed by Repoint (a
        // cast claim's Repoint renews its window) well before its 15 s window ends.
        {
            auto& cm = apmf::ControlMap::Get();
            if (!cm.IsClaimLive(a_sub.floorR) || !cm.IsClaimLive(a_sub.floorL)) {
                CloseSub(a_sub, "a hand floor is no longer live (the hands would be undenied)");
                return false;
            }
            if (a_now - a_sub.floorRenewMs >= kFloorRenewMs) {
                a_sub.floorRenewMs = a_now;
                APMF_API::APMF_Param fp{};
                fp.ival = static_cast<std::int32_t>(APMF_API::kCastFlag_DenyHandOnly);
                cm.EnqueueRepoint(a_sub.floorR, &fp);
                fp.ival = static_cast<std::int32_t>(APMF_API::kCastFlag_DenyHandOnly | APMF_API::kCastFlag_LeftHand);
                cm.EnqueueRepoint(a_sub.floorL, &fp);
            }
        }

        // P3: what the world did, sampled every frame while open
        auto* proc = actor->GetActorRuntimeData().currentProcess;
        if (proc && a_sub.bitPending) WriteBitAfterRepick(a_sub, actor, proc, a_now);
        if (proc) {
            if (proc->lowProcessFlags.underlying() & 0x04) a_sub.aggressorSeen = true;   // AIProcess+0x136 bit 2
            const bool on = proc->ignoringCombat;
            if (a_sub.bitSet && a_sub.bitWasOn && !on) {   // the engine re-synced the bit from the package flag
                ++a_sub.bitResets;
                // review F2: attribute it. A package (re)start runs UpdateIgnoringCombat (AE 0x6D7E80 from 39434,
                // the start path under 39213); a changed package or start time since the write says that was it.
                const bool restart = proc->currentPackage.package != a_sub.bitPkg ||
                                     proc->currentPackage.packageStartTime != a_sub.bitPkgStart;
                restart ? ++a_sub.resetsByRestart : ++a_sub.resetsOther;
                if (!restart) apmf::substrateprobe::OtherReset(a_sub.id, proc, a_now);   // R1
                apmf::substratepackage::Reassert(a_sub.id, proc, restart, a_now - a_sub.openedMs);   // S2b (F-6 round)
                if (!a_sub.firstResetMs) {
                    a_sub.firstResetMs = a_now;
                    spdlog::info("[substrate] 0x{} ignoring-bit reset by the engine at +{} ms ({}, cause: {}) -- S2b "
                                 "re-asserts each reset while [CombatSubstrate] bSubstrateBitReassert=1.",
                                 Hex(a_sub.id), a_now - a_sub.openedMs, a_sub.bitResets,
                                 restart ? "a package (re)start: the package or its start time changed since the write"
                                         : "OTHER: the same package, the same start time");
                }
            }
            a_sub.bitWasOn = proc->ignoringCombat;   // S2b may have written it back above
        }
        // the probe: the FIRST decided state of his group, recorded by the seat inside the engine's own update of it
        // (review F6: the game thread never dereferences the group for music)
        if (const std::uint32_t mu = s.music.load(std::memory_order_acquire); !a_sub.musicDecidedLogged && (mu & kMusSeen)) {
            a_sub.musicDecidedLogged = true;
            const std::uint64_t at   = s.musicAtMs.load(std::memory_order_relaxed);
            spdlog::info("[substrate] 0x{} group music state DECIDED = {} at +{} ms after the entry, by {}.", Hex(a_sub.id),
                         MusicName(static_cast<std::uint8_t>(mu & 0xFF)), at > a_sub.openedMs ? at - a_sub.openedMs : 0,
                         (mu & kMusWrote) && (mu & 0xFF) == kMusicNone ? "Harbinger's music deny" : "the ENGINE");
        }
        if (!a_sub.combatEventLogged && s.combatEvents.load(std::memory_order_relaxed) != 0) {
            a_sub.combatEventLogged = true;
            spdlog::info("[substrate] 0x{} known hole: combat state event sent (TESCombatEvent -> Papyrus "
                         "OnCombatStateChanged for the holder; approved by marth 2026-10-12, never hidden from other "
                         "mods' scripts).",
                         Hex(a_sub.id));
        }

        // P6: the MOVE trace
        if (g_moveTrace && a_now - a_sub.lastTraceMs >= kTraceEveryMs) {
            a_sub.lastTraceMs = a_now;
            const RE::NiPoint3 pos = actor->GetPosition();
            const float dt = static_cast<float>(a_now - a_sub.lastPosMs) / 1000.0f;
            const float speed = dt > 0.0f ? pos.GetDistance(a_sub.lastPos) / dt : 0.0f;
            a_sub.lastPos   = pos;
            a_sub.lastPosMs = a_now;
            std::uint32_t type = 0;
            const RE::FormID pkg = PackageOf(actor, &type);
            auto* pc   = RE::PlayerCharacter::GetSingleton();
            auto* cc   = actor->GetActorRuntimeData().combatController;
            spdlog::info("[substrate] 0x{} MOVE pkg=0x{} proc={} ctrl+0x42={} proc+0x139={} speed={:.0f} "
                         "dist-to-pkg-goal={:.0f} (the player) movement-leaves-denied={} ignoring-bit reset by the engine ({}: "
                         "package restart {}, other {}){}",
                         Hex(a_sub.id), Hex(pkg), type, cc && cc->ignoringCombat ? 1 : 0,
                         proc && proc->ignoringCombat ? 1 : 0, static_cast<double>(speed),
                         pc ? static_cast<double>(pos.GetDistance(pc->GetPosition())) : -1.0,
                         s.moveDenied.load(std::memory_order_relaxed), a_sub.bitResets, a_sub.resetsByRestart,
                         a_sub.resetsOther, a_sub.bitPending ? " [bit not yet written: waiting for the re-pick]" : "");
        }
        return true;
    }

    // design 3.3: the anchor stands at the holder with the shortest-range claim (earliest on a tie);
    // every other holder farther than its own reach logs one wait line.
    void PlaceAnchor() {
        if (g_anchorOut == 0) return;
        const Sub* best = nullptr;
        for (const auto& [id, sub] : g_subs) {
            if (sub.phase != Phase::kOpen || sub.slot < 0) continue;
            if (!best || sub.tier < best->tier || (sub.tier == best->tier && sub.openedMs < best->openedMs)) best = &sub;
        }
        if (!best) return;
        auto* anchor = Anchor();
        auto* at     = RE::TESForm::LookupByID<RE::Actor>(best->id);
        if (!anchor || !at) return;
        const bool moved = anchor->GetParentCell() != at->GetParentCell() ||
                           anchor->GetPosition().GetDistance(BehindOf(at)) > kAnchorFollowU;
        if (best->id != g_served || moved) {
            AnchorToHolder(anchor, at, false);
            g_served = best->id;
        } else {
            // review F3 of a512763: SetAlpha (slot 0xE1, AE 0x6C1A50 / SE 0x62FAD0) returns at once unless the
            // anchor is HIGH process (AE 0x69C6D0 / SE 0x608DE0), which it is not in the frame it leaves its unloaded
            // holding cell. So it is applied on EVERY Poll while the anchor is out, not only on a move.
            anchor->SetAlpha(0.0f);
        }
        if (auto* ap = anchor->GetActorRuntimeData().currentProcess; ap && ap->InHighProcess())
            for (auto& [id, sub] : g_subs)
                if (sub.phase == Phase::kOpen) sub.anchorHighSeen = true;
        for (auto& [id, sub] : g_subs) {
            if (sub.phase != Phase::kOpen || sub.slot < 0 || id == g_served) continue;
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!a) continue;
            const float d = a->GetPosition().GetDistance(anchor->GetPosition());
            if (d > kTierReach[sub.tier] && sub.waitLoggedFor != g_served) {
                sub.waitLoggedFor = g_served;
                spdlog::info("[substrate] 0x{} waits: anchor serves 0x{} (range {:.0f}; this holder's {} claim reaches "
                             "{:.0f}, it is {:.0f} from the anchor). Its claim is not cancelled.",
                             Hex(id), Hex(g_served), static_cast<double>(kTierReach[best->tier]), kTierName[sub.tier],
                             static_cast<double>(kTierReach[sub.tier]), static_cast<double>(d));
            }
        }
    }

    // trigger (c) and the game-thread half of (a): the party in combat, a non-anchor group target.
    void PartyPoll(std::uint64_t a_now) {
        if (a_now - g_lastPartyMs < kPartyPollMs) return;
        g_lastPartyMs = a_now;
        const std::uint32_t anchorH = g_anchorH.load(std::memory_order_acquire);
        RE::FormID          partyFoe = 0;
        const bool          playerFight = PlayerInCombat();
        if (!playerFight) {
            if (auto* pl = RE::ProcessLists::GetSingleton()) {
                pl->ForEachHighActor([&](RE::Actor* a) {
                    if (!a || !a->IsPlayerTeammate()) return RE::BSContainer::ForEachResult::kContinue;
                    std::uint32_t st = 0;
                    if (FindSlot(a->GetFormID(), st) >= 0 && (st & (kStLive | kStReal)) == kStLive)
                        return RE::BSContainer::ForEachResult::kContinue;   // a holder under its substrate
                    auto* cc = a->GetActorRuntimeData().combatController;
                    if (!cc) return RE::BSContainer::ForEachResult::kContinue;
                    const auto th = cc->targetHandle.native_handle();
                    if (th != 0 && th != anchorH) {
                        partyFoe = a->GetFormID();
                        return RE::BSContainer::ForEachResult::kStop;
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
        }
        for (auto& [id, sub] : g_subs) {
            if (sub.phase != Phase::kOpen || sub.slot < 0) continue;
            if (playerFight) sub.playerCombatSeen = true;
            if (playerFight || partyFoe) {
                MarkReal(sub.slot, kWhyParty, playerFight ? 0x14 : partyFoe);
                continue;
            }
            auto* a  = RE::TESForm::LookupByID<RE::Actor>(id);
            auto* cc = a ? a->GetActorRuntimeData().combatController : nullptr;
            auto* g  = cc ? cc->combatGroup : nullptr;
            if (!g) continue;
            bool            other = false;
            RE::ActorHandle otherH{};
            {
                RE::BSReadLockGuard guard(g->lock);
                if (const auto* t = FirstNonAnchorTarget(g, anchorH)) {
                    other  = true;
                    otherH = t->targetHandle;   // copied out; resolved after the lock
                }
            }
            if (other) {
                const auto op = otherH.get();
                MarkReal(sub.slot, kWhyAddedTarget, op ? op->GetFormID() : 0);
            }
        }
    }

    // Why a player teammate is NOT idle enough for the dry run to open a substrate on (nullptr = idle).
    // A substrate gives the actor a CombatController, and a walking / sitting / talking follower must
    // not be dragged into one (field 2026-10-19: the dry run cancelled every ch.19 town walk).
    // Claims are read through the RCU snapshot (any thread); the engine reads are plain member reads.
    bool        g_dialogueOk = false;   // the MenuTopicManager singleton row verified (Install)
    const char* NotIdleReason(RE::Actor* a) {
        auto& cm = apmf::ControlMap::Get();
        APMF_API::APMF_Param p{};
        if (cm.TryGetOwningClaim(a->GetFormID(), APMF_API::kIntent_Travel, p)) return "owned ch.19 travel claim";
        if (cm.TryGetOwningClaim(a->GetFormID(), APMF_API::kIntent_OfferPackage, p)) return "owned ch.9 offer-package claim";
        if (cm.TryGetOwningClaim(a->GetFormID(), APMF_API::kIntent_MovementBlock, p)) return "owned ch.1 hold claim";
        if (cm.TryGetOwningClaim(a->GetFormID(), APMF_API::kIntent_Idle, p)) return "owned ch.12 idle claim";
        if (const auto* as = a->AsActorState()) {
            if (as->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) return "sitting / sleeping / in furniture";
        }
        // IN DIALOGUE WITH THE PLAYER: the engine's own test (Papyrus IsInDialogueWithPlayer, AE 56194 / SE 55663, verified
        // row Substrate.MenuTopicManager.Reader): MenuTopicManager::speaker (+0x68 on both runtimes) == the ref's handle.
        // (Not NPC-to-NPC. ActorState2::talkingToPlayer is NOT read: nothing in the executable tests that bit.)
        if (!g_dialogueOk) return "dialogue state unknown (MenuTopicManager row unverified)";
        if (const auto* mtm = RE::MenuTopicManager::GetSingleton();
            mtm && mtm->speaker.native_handle() != 0 && mtm->speaker.native_handle() == a->GetHandle().native_handle())
            return "in dialogue with the player";
        if (a->GetOccupiedFurniture()) return "occupying furniture";
        return nullptr;
    }

    void DryRunSchedule(std::uint64_t a_now) {
        if (g_dryRun)   // R1: the baseline of the next pick round, from 1 s before it
            apmf::substrateprobe::BeforeDryRound(a_now, g_lastDryMs + static_cast<std::uint64_t>(g_dryPeriodSec * 1000.0f));
        if (!g_dryRun || a_now - g_lastDryMs < static_cast<std::uint64_t>(g_dryPeriodSec * 1000.0f)) return;
        g_lastDryMs = a_now;
        std::vector<RE::FormID> picks;
        std::string             skipped;   // "0x<id> (<reason>), ..." -- one line per pick round, never per frame
        if (auto* pl = RE::ProcessLists::GetSingleton()) {
            pl->ForEachHighActor([&](RE::Actor* a) {
                if (a && a->IsPlayerTeammate() && !a->IsDead() && a->Is3DLoaded() && !HasController(a) &&
                    !g_subs.contains(a->GetFormID())) {
                    if (const char* why = NotIdleReason(a))
                        skipped += fmt::format("{}0x{} ({})", skipped.empty() ? "" : ", ", Hex(a->GetFormID()), why);
                    else
                        picks.push_back(a->GetFormID());
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        if (!skipped.empty())
            spdlog::info("[substrate] dry run: skipped as not idle: {}.", skipped);
        if (picks.empty()) {
            spdlog::info("[substrate] dry run: no idle loaded player teammate to open a substrate on (next try in {:.0f} s).",
                         static_cast<double>(g_dryPeriodSec));
            return;
        }
        for (const auto id : picks) {
            Sub s{};
            s.id          = id;
            s.dryRun      = true;
            s.requestedMs = a_now;
            s.ttlMs       = TtlMsFor(0.0f);
            s.deadlineMs  = a_now + s.ttlMs + kFloorWaitMs;
            g_subs.emplace(id, s);
            apmf::substrateprobe::Requested(id, a_now);   // R1
            spdlog::info("[substrate] 0x{} DRY RUN requested (no claim, no cast; TTL {:.1f} s).", Hex(id),
                         static_cast<double>(s.ttlMs) / 1000.0);
        }
        g_subCount.store(g_subs.size(), std::memory_order_relaxed);
    }

    // The music probe's change log (see "the music probe" above). Game thread, every Poll; one map walk and
    // nothing else while no substrate is open and the tail has run out.
    void MusicProbe(std::uint64_t a_now) {
        if (!g_musicOk) return;
        std::string open;
        for (const auto& [id, sub] : g_subs)
            if (sub.phase == Phase::kOpen) open += fmt::format("{}0x{}", open.empty() ? "" : ",", Hex(id));
        if (!open.empty()) {
            g_musicLastOpenMs = a_now;
            g_musicTrackUntil = a_now + kMusicTailMs;
        } else if (!g_musicTracking) {
            return;
        }
        const RE::FormID m = MusicNow();
        if (!g_musicTracking) {
            g_musicTracking = true;
            g_musicLast     = m;
            spdlog::info("[substrate] MUSIC tracking starts: current 0x{} (open: {}; player-in-combat={}).", Hex(m), open,
                         PlayerInCombat() ? "y" : "n");
            return;
        }
        if (m != g_musicLast) {
            spdlog::info("[substrate] MUSIC 0x{} -> 0x{} ({}; player-in-combat={}).", Hex(g_musicLast), Hex(m),
                         open.empty() ? fmt::format("no substrate open: +{} ms after the last one closed",
                                                    a_now - g_musicLastOpenMs)
                                      : fmt::format("open: {}", open),
                         PlayerInCombat() ? "y" : "n");
            g_musicLast = m;
            for (auto& [id, sub] : g_subs)
                if (sub.phase == Phase::kOpen) ++sub.musicChanges;
        }
        if (open.empty() && a_now >= g_musicTrackUntil) {
            g_musicTracking = false;
            spdlog::info("[substrate] MUSIC tracking ends ({} s after the last substrate closed): current 0x{}.",
                         kMusicTailMs / 1000, Hex(m));
        }
    }

    // ---- the BYSTANDER CENSUS (2026-10-19; PASSIVE: no write, no hook, no address id) -------------------
    // Field: townsfolk panicked while a substrate was open and no crime was committed; which channel does it is
    // not attributed. While any substrate is open, and kCensusTailMs after the last one closes, walk the high-process
    // actors that are not the player, a live holder or the anchor and stand within kCensusRadius of a holder (since
    // 2026-10-19 NON-HOLDER teammates included: the SEV-1 leak was a teammate; an actor whose controller targets the
    // anchor is logged at any distance and keeps the window open), and log
    // ON CHANGE ONLY what each one's engine state does. Every read is a plain member read of a CommonLib member
    // (CombatController, CombatState, AIProcess+0x136, the running package's type); NOT instrumented: the combat-start
    // topic and the alarm / detection level (no verified accessor). Game thread, Poll.
    constexpr std::uint64_t kCensusTailMs   = 10000;
    constexpr float         kCensusRadius   = 2048.0f;
    constexpr std::uint32_t kCensusMaxLines = 60;   // per window; then ONE summary line and silence
    struct CensusRow {
        bool          ctrl = false, inGroup = false, fleeing = false, detected = false, aggr = false, alarm = false,
                      tgtAnchor = false;
        std::uint32_t tgt = 0;
        bool operator==(const CensusRow&) const = default;
    };
    std::unordered_map<RE::FormID, CensusRow> g_censusRows;
    bool          g_censusLive = false;        // a window is running
    std::uint64_t g_censusUntil = 0, g_censusLastOpenMs = 0;
    std::uint32_t g_censusLines = 0, g_censusSuppressed = 0;

    void CensusEnd(const char* a_why) {
        if (!g_censusLive) return;
        spdlog::info("[substrate] CENSUS window ends ({}): {} line(s) logged, {} suppressed by the cap ({}).", a_why,
                     g_censusLines, g_censusSuppressed, kCensusMaxLines);
        g_censusLive = false;
        g_censusRows.clear();
    }

    void BystanderCensus(std::uint64_t a_now) {
        if (!g_census) return;
        std::vector<RE::NiPoint3>   holders;
        std::vector<RE::CombatGroup*> groups;
        for (const auto& [id, sub] : g_subs) {
            if (sub.phase != Phase::kOpen) continue;
            g_censusLastOpenMs = std::max(g_censusLastOpenMs, sub.openedMs);
            if (auto* h = RE::TESForm::LookupByID<RE::Actor>(id)) {
                holders.push_back(h->GetPosition());
                if (auto* cc = h->GetActorRuntimeData().combatController; cc && cc->combatGroup)
                    groups.push_back(cc->combatGroup);   // compared by address only, never dereferenced
            }
        }
        if (!holders.empty()) {
            g_censusUntil = a_now + kCensusTailMs;
            if (!g_censusLive) {
                g_censusLive = true;
                g_censusLines = g_censusSuppressed = 0;
                g_censusRows.clear();
                spdlog::info("[substrate] CENSUS window opens ({} holder(s)): bystanders within {:.0f} u, change-only.",
                             holders.size(), static_cast<double>(kCensusRadius));
            }
        } else if (!g_censusLive) {
            return;
        } else if (a_now >= g_censusUntil) {
            CensusEnd("tail over");
            return;
        }
        // After the last close the holders are gone: the radius test uses the last positions seen.
        static std::vector<RE::NiPoint3> s_lastHolders;
        if (!holders.empty()) s_lastHolders = holders;
        const auto& around = holders.empty() ? s_lastHolders : holders;
        const auto  aid    = g_anchorId.load(std::memory_order_relaxed);
        const auto  aH     = g_anchorH.load(std::memory_order_acquire);
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!pl) return;
        const std::uint64_t sinceOpen = a_now - g_censusLastOpenMs;
        bool                anyAnchor = false;   // some actor's controller targets the anchor: the window stays open
        pl->ForEachHighActor([&](RE::Actor* a) {
            if (!a || a->IsPlayerRef() || a->GetFormID() == aid || a->IsDead())
                return RE::BSContainer::ForEachResult::kContinue;
            std::uint32_t hst = 0;
            if (FindSlot(a->GetFormID(), hst) >= 0 && (hst & kStLive))   // a live holder: not a bystander
                return RE::BSContainer::ForEachResult::kContinue;
            auto*      acc = a->GetActorRuntimeData().combatController;
            const bool tA  = acc && aH != 0 && acc->targetHandle.native_handle() == aH;
            if (tA) anyAnchor = true;
            const auto pos = a->GetPosition();
            bool       near = tA;
            for (const auto& hp : around)
                if (near || pos.GetDistance(hp) <= kCensusRadius) { near = true; break; }
            const auto id = a->GetFormID();
            if (!near) {
                g_censusRows.erase(id);
                return RE::BSContainer::ForEachResult::kContinue;
            }
            CensusRow r{};
            auto&     rt = a->GetActorRuntimeData();
            r.tgtAnchor = tA;
            if (auto* cc = rt.combatController) {
                r.ctrl = true;
                if (const auto t = cc->targetHandle.get()) r.tgt = t->GetFormID();
                r.inGroup = cc->combatGroup && std::find(groups.begin(), groups.end(), cc->combatGroup) != groups.end();
                if (cc->state) {
                    r.fleeing  = cc->state->isFleeing;
                    r.detected = cc->state->isDetected;
                }
            }
            if (auto* proc = rt.currentProcess) {
                r.aggr = (proc->lowProcessFlags.underlying() & 0x04) != 0;   // AIProcess+0x136 bit 2
                if (const auto* pk = proc->GetRunningPackage())
                    r.alarm = pk->packData.packType.get() == RE::PACKAGE_PROCEDURE_TYPE::kAlarm;
            }
            auto& old = g_censusRows[id];
            if (r == old) return RE::BSContainer::ForEachResult::kContinue;
            if (g_censusLines >= kCensusMaxLines) {
                if (g_censusSuppressed++ == 0)
                    spdlog::info("[substrate] CENSUS cap of {} line(s) reached for this window; further changes are "
                                 "counted, not logged.", kCensusMaxLines);
                old = r;
                return RE::BSContainer::ForEachResult::kContinue;
            }
            ++g_censusLines;
            spdlog::info("[substrate] CENSUS 0x{} ({}{}): controller {}->{}, target 0x{} (was 0x{}), target-is-anchor {}->{}, "
                         "in-holder-group {}->{}, fleeing {}->{}, combat-detected {}->{}, aggressor-bit {}->{}, alarm-package "
                         "{}->{}; +{} ms since the latest OPEN.",
                         Hex(id), a->GetName(), a->IsPlayerTeammate() ? ", teammate, not a holder" : "", old.ctrl, r.ctrl,
                         Hex(r.tgt), Hex(old.tgt), old.tgtAnchor, r.tgtAnchor, old.inGroup, r.inGroup, old.fleeing,
                         r.fleeing, old.detected, r.detected, old.aggr, r.aggr, old.alarm, r.alarm, sinceOpen);
            old = r;
            return RE::BSContainer::ForEachResult::kContinue;
        });
        if (anyAnchor) g_censusUntil = std::max(g_censusUntil, a_now + kCensusTailMs);
    }

    // Debts a drop timed out with no real target seen in a group holding the holder (game thread, Poll).
    void DebtExpire(std::uint64_t a_now) {
        if (g_debtCount.load(std::memory_order_acquire) == 0) return;
        std::vector<RE::FormID> gone;
        {
            std::scoped_lock lk(g_debtMx);
            for (auto& d : g_debts)
                if (d.holderH != 0 && d.expiresMs != 0 && a_now >= d.expiresMs) {
                    gone.push_back(d.holder);
                    d = MusicDebt{};
                }
            if (!gone.empty()) DebtCountLocked();
        }
        for (const auto id : gone)
            spdlog::info("[substrate] 0x{} music debt ended: no group holding him showed a real target within {} s of the "
                         "drop (his denied group ended with its fight; nothing to put back).",
                         Hex(id), kDebtAfterDropMs / 1000);
    }

    void Heartbeat(std::uint64_t a_now) {
        if (a_now - g_lastHeartbeatMs < kHeartbeatMs) return;
        g_lastHeartbeatMs = a_now;
        spdlog::info("[substrate] HEARTBEAT open now {} (anchor out for {}, served 0x{}) | opened {} closed {} dropped "
                     "(real combat) {} not opened {} entry refused {} | anchor StartCombat refusals {} | dry run {}",
                     g_subs.size(), g_anchorOut, Hex(g_served), g_opened, g_closed, g_drops, g_refusedOpen,
                     g_entryRefused, g_anchorRefusals.load(std::memory_order_relaxed), g_dryRun ? "ON" : "off");
    }

    // ---- the event sinks: triggers (b) and the combat-state hole --------------------------------
    class HitSink final : public RE::BSTEventSink<RE::TESHitEvent> {
    public:
        static HitSink* Get() { static HitSink s; return &s; }
        RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_e, RE::BSTEventSource<RE::TESHitEvent>*) override {
            try {
                if (!a_e || !subs::AnyLive() || !a_e->target) return RE::BSEventNotifyControl::kContinue;
                std::uint32_t st = 0;
                const int     si = FindSlot(a_e->target->GetFormID(), st);
                if (si < 0 || (st & (kStLive | kStReal)) != kStLive) return RE::BSEventNotifyControl::kContinue;
                g_slots[si].hits.fetch_add(1, std::memory_order_relaxed);
                auto* cause = a_e->cause ? a_e->cause->As<RE::Actor>() : nullptr;
                if (!cause) return RE::BSEventNotifyControl::kContinue;   // no actor (a trap, the world): not a foe
                std::uint32_t cst = 0;
                const bool party = cause->IsPlayerRef() || cause->IsPlayerTeammate() ||
                                   cause->GetFormID() == a_e->target->GetFormID() ||
                                   FindSlot(cause->GetFormID(), cst) >= 0;
                if (!party) MarkReal(si, kWhyHit, cause->GetFormID());
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class CombatEventSink final : public RE::BSTEventSink<RE::TESCombatEvent> {
    public:
        static CombatEventSink* Get() { static CombatEventSink s; return &s; }
        RE::BSEventNotifyControl ProcessEvent(const RE::TESCombatEvent* a_e,
                                              RE::BSTEventSource<RE::TESCombatEvent>*) override {
            try {
                if (!a_e || !subs::AnyLive() || !a_e->actor) return RE::BSEventNotifyControl::kContinue;
                std::uint32_t st = 0;
                const int     si = FindSlot(a_e->actor->GetFormID(), st);
                if (si >= 0) g_slots[si].combatEvents.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // fix/apmf-substrate-release-close (APMF-B93): the release evidence. The holder's TESSpellCastEvent for the form its
    // DRIVING claim names (the spell or its proxy) is recorded on its slot (the first one per window); Serve closes the
    // window as "fired" on it. Any thread (the source's own dispatch); the slot table and the RCU claim read only.
    class SpellCastSink final : public RE::BSTEventSink<RE::TESSpellCastEvent> {
    public:
        static SpellCastSink* Get() { static SpellCastSink s; return &s; }
        RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_e,
                                              RE::BSTEventSource<RE::TESSpellCastEvent>*) override {
            try {
                if (!a_e || !subs::AnyLive() || !a_e->object || a_e->spell == 0) return RE::BSEventNotifyControl::kContinue;
                const RE::FormID holder = a_e->object->GetFormID();
                std::uint32_t    st     = 0;
                const int        si     = FindSlot(holder, st);
                if (si < 0 || (st & (kStLive | kStReal)) != kStLive) return RE::BSEventNotifyControl::kContinue;
                apmf::CastSeatClaim c{};
                if (!DrivingClaim(holder, c) || (a_e->spell != c.spell && a_e->spell != c.proxy))
                    return RE::BSEventNotifyControl::kContinue;
                RE::FormID zero = 0;
                if (g_slots[si].castEventForm.compare_exchange_strong(zero, a_e->spell, std::memory_order_acq_rel))
                    g_slots[si].castEventMs.store(apmf::clock::MonotonicMs(), std::memory_order_relaxed);
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ---- the channel ------------------------------------------------------------------------
    class CombatSubstrateChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "combat-substrate"; }
        int              ChannelNo() const override { return 27; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_CombatSubstrate; }

        // Inside Drain: record the request only. Opening files internal claims (ControlMap writes), so it
        // runs from Poll, outside Drain (core/Channel.h).
        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            Declare(id, param, "ENGAGED");
        }
        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            Declare(id, param, "RE-DECLARED");
        }
        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            const auto it = g_subs.find(id);
            if (it == g_subs.end()) return;
            it->second.closeAsked = true;
            it->second.closeWhy   = "client release";
            it->second.handle     = APMF_API::kInvalidHandle;   // released: nothing for Harbinger to release
        }

    private:
        static void Declare(RE::FormID id, const APMF_API::APMF_Param& param, const char* what) {
            const auto now = apmf::clock::MonotonicMs();
            auto [it, fresh] = g_subs.try_emplace(id);
            Sub& s = it->second;
            if (fresh) {
                s.id          = id;
                s.requestedMs = now;
            } else if (s.dryRun) {
                s.dryRun = false;   // a client's claim takes over a dry-run substrate on the same actor
            }
            s.closeAsked = false;
            s.closeWhy   = nullptr;
            s.ttlMs      = TtlMsFor(param.fval);
            // The winning handle is read after Drain publishes (Poll); a TTL runs from this declaration.
            s.deadlineMs = now + s.ttlMs + (s.phase == Phase::kOpen ? 0 : kFloorWaitMs);
            g_subCount.store(g_subs.size(), std::memory_order_relaxed);
            spdlog::info("[substrate] 0x{} substrate {} (TTL {:.1f} s{}).", Hex(id), what,
                         static_cast<double>(s.ttlMs) / 1000.0,
                         param.fval > kMaxTtlSec ? fmt::format(", param.fval {:.1f} clamped to {:.0f} s",
                                                               static_cast<double>(param.fval), static_cast<double>(kMaxTtlSec))
                                                 : std::string());
        }
    };

    static_assert(offsetof(RE::AIProcess, ignoringCombat) == 0x139, "AIProcess+0x139 is the process ignoring-combat bit "
                                                                     "(AE 0x6D6780 / SE 0x643F00 read it): re-verify");
    static_assert(offsetof(RE::AIProcess, lowProcessFlags) == 0x136, "AIProcess+0x136 (the aggressor bit 2): re-verify");
    static_assert(offsetof(RE::AIProcess, currentPackage) == 0x18 && offsetof(RE::ActorPackage, package) == 0x08 &&
                      offsetof(RE::ActorPackage, packageStartTime) == 0x20,
                  "UpdateIgnoringCombat (AE 0x6D7E80) reads process+0x18 (the ActorPackage) +0x08 / +0x24: re-verify; "
                  "+0x20 packageStartTime is CommonLib's name, read for the P6 reset attribution only");
    static_assert(static_cast<std::uint32_t>(RE::Actor::BOOL_BITS::kEvpBuffered) == (1u << 16),
                  "EvaluatePackage's deferred branch sets boolBits bit 16 (AE 0x66D13C / SE 0x5DB732): re-verify");
    static_assert(offsetof(RE::CombatController, ignoringCombat) == 0x42, "CombatController+0x42 (paused; AE 0x55A870 "
                                                                          "writes it): re-verify");
    static_assert(offsetof(RE::CombatController, combatGroup) == 0x00 &&
                  offsetof(RE::CombatController, targetHandle) == 0x2C, "CombatController layout below +0x68: re-verify");
    static_assert(offsetof(RE::CombatGroup, targets) == 0x08 && offsetof(RE::CombatGroup, members) == 0x20 &&
                  offsetof(RE::CombatGroup, musicState) == 0x15C && offsetof(RE::CombatGroup, lock) == 0x160,
                  "CombatGroup layout (the hold's, re-measured): re-verify");
    static_assert(offsetof(RE::CombatTarget, flags) == 0xA6 && sizeof(RE::CombatTarget) == 0xA8,
                  "CombatTarget layout (the hold's): re-verify");
    static_assert(apmf::cbt::kLeaves.size() + apmf::cbt::kSearchLeaves.size() + apmf::cbt::kParamLeaves.size() ==
                      subs::kLeafCount,
                  "the allow-list's index must cover the 70-leaf catalog, the four search leaves and the 21 "
                  "parameterised nodes (every NodeObject* vtable the fork ships)");

    // ---- 1.7.104 (marth 2026-10-19 Q4: "substrate unavailable while Harbinger present shouldn't happen") ----
    // The AE ids 44705 (RemoveTarget), 44703 (the group update), 46888 (the should-play test), 36550 (the
    // BSMusicManager reader) and 56194 (IsInDialogueWithPlayer) have NO row in the fork's MIT id table, and a
    // RELOCATION_ID miss is FATAL on 1.7.104. So on 1.7.104 these addresses are raw-RVA rows of the 1.7.104
    // self-check table (tools/verified_addresses/idmap-1.7.104-apmf.csv, rows Substrate.*.1_7_104; each one the
    // 1.6.1170 signature cut at the AE address, unique in the 1.7.104 .text at exactly this RVA). Every function
    // here is instruction-identical to its 1.6.1170 twin (call / RIP targets normalised) at the SAME offsets, so the
    // AE offsets hold; Docs/VERIFIED-ADDRESSES.md "Combat substrate on 1.7.104" has the per-row proof. A raw row
    // carries no byte check, so the code re-reads the bytes each fact rests on (the 1.6.1170 / 1.5.97 rows have
    // SelfCheck do that): the two E8 targets, the two `cmp byte [reg+0x15C], imm` encodings, and each reader's RIP
    // displacement, which must land on the singleton the fork's MIT table resolves (ids 400896 / 401099 ARE in it).
    constexpr std::uintptr_t k17RemoveTarget          = 0x818BD0;   // AE 0x803D70
    constexpr std::uintptr_t k17GroupUpdate           = 0x818570;   // AE 0x803710 (id 44703)
    constexpr std::uintptr_t k17MusicWriteLock        = 0x8185A0;   // update + 0x30: E8 -> BSReadWriteLock::LockForWrite
    constexpr std::uintptr_t k17DetectionPassInUpdate = 0x8185DD;   // update + 0x6D: E8 -> the group detection pass
    constexpr std::uintptr_t k17DetectionPass         = 0x821CE0;   // AE 0x80CE00 (id 44804)
    constexpr std::uintptr_t k17MusicUndecided        = 0x8187AC;   // update + 0x23C: cmp byte [rdi+0x15C], 0xFF
    constexpr std::uintptr_t k17MusicPlayTest         = 0x852FD9;   // 0x852DF0 (AE 0x83DF10, id 46888) + 0x1E9
    constexpr std::uintptr_t k17MusicReader           = 0x65454F;   // 0x654480 (AE 0x641AD0, id 36550) + 0xCF
    constexpr std::uintptr_t k17MenuTopicReader       = 0xA46267;   // 0xA46240 (AE 0xA2D7A0, id 56194) + 0x27
    static_assert(k17MusicWriteLock == k17GroupUpdate + 0x30 && k17DetectionPassInUpdate == k17GroupUpdate + 0x6D &&
                  k17MusicUndecided == k17GroupUpdate + 0x23C && k17MusicPlayTest == 0x852DF0 + 0x1E9 &&
                  k17MusicReader == 0x654480 + 0xCF && k17MenuTopicReader == 0xA46240 + 0x27);

    // The bytes at a_addr are a_want. Logs the first mismatch by name.
    bool BytesAt17(std::uintptr_t a_addr, std::span<const std::uint8_t> a_want, const char* a_seat) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(a_addr);
        for (std::size_t i = 0; i < a_want.size(); ++i) {
            if (b[i] != a_want[i]) {
                spdlog::error("[selfcheck] seat {} REFUSED on 1.7.104: byte +{} at RVA 0x{:X} is 0x{:02X}, expected 0x{:02X}",
                              a_seat, i, a_addr - REL::Module::get().base(), b[i], a_want[i]);
                return false;
            }
        }
        return true;
    }

    // An E8 at a_site that lands on a_target.
    bool CallsTo17(std::uintptr_t a_site, std::uintptr_t a_target, const char* a_seat) {
        const auto*  b   = reinterpret_cast<const std::uint8_t*>(a_site);
        std::int32_t rel = 0;
        std::memcpy(&rel, b + 1, sizeof(rel));
        const std::uintptr_t lands = a_site + 5 + static_cast<std::intptr_t>(rel);
        if (b[0] == 0xE8 && lands == a_target) return true;
        spdlog::error("[selfcheck] seat {} REFUSED on 1.7.104: RVA 0x{:X} is {:02X} -> RVA 0x{:X}, expected E8 -> RVA 0x{:X}",
                      a_seat, a_site - REL::Module::get().base(), b[0], lands - REL::Module::get().base(),
                      a_target - REL::Module::get().base());
        return false;
    }

    // A verified raw reader row whose `mov reg, [rip+disp32]` (7 bytes; a_head = REX + opcode + ModRM, a_tail = the
    // bytes after it) reads exactly a_global: the 1.7.104 form of the generator's ripref proof (reader -> global).
    bool ReaderReads17(std::uintptr_t a_rva, std::array<std::uint8_t, 3> a_head, std::array<std::uint8_t, 8> a_tail,
                       std::uintptr_t a_global, const char* a_seat) {
        const std::uintptr_t site = REL::Offset(a_rva).address();
        if (!apmf::allowance::SeatVerified(site, a_seat)) return false;
        const auto* b = reinterpret_cast<const std::uint8_t*>(site);
        if (std::memcmp(b, a_head.data(), a_head.size()) != 0 || std::memcmp(b + 7, a_tail.data(), a_tail.size()) != 0) {
            spdlog::error("[selfcheck] seat {} REFUSED on 1.7.104: the reader at RVA 0x{:X} is not the verified "
                          "`mov reg, [rip+disp32]` encoding",
                          a_seat, a_rva);
            return false;
        }
        std::int32_t disp = 0;
        std::memcpy(&disp, b + 3, sizeof(disp));
        const std::uintptr_t reads = site + 7 + static_cast<std::intptr_t>(disp);
        if (reads == a_global) return true;
        spdlog::error("[selfcheck] seat {} REFUSED on 1.7.104: the reader at RVA 0x{:X} reads RVA 0x{:X}, the singleton is "
                      "RVA 0x{:X}",
                      a_seat, a_rva, reads - REL::Module::get().base(), a_global - REL::Module::get().base());
        return false;
    }

}

namespace apmf::combatsubstrate {

    bool        Installed() { return g_installed.load(std::memory_order_acquire); }
    const char* NotInstalledReason() {
        const char* r = g_notInstalledReason.load(std::memory_order_acquire);
        return r ? r : "unknown";
    }
    bool       AnyLive() { return g_slotCount.load(std::memory_order_relaxed) != 0; }
    std::uint32_t WindowSerial() { return g_windowSerial.load(std::memory_order_relaxed); }
    RE::FormID AnchorId() { return g_anchorId.load(std::memory_order_relaxed); }

    bool Suppressing(RE::FormID a_actor) {
        if (!AnyLive()) return false;
        std::uint32_t st = 0;
        return FindSlot(a_actor, st) >= 0 && (st & (kStLive | kStReal)) == kStLive;
    }

    std::uint32_t StateOf(RE::FormID a_actor) {
        if (a_actor == 0 || !Installed()) return APMF_API::kSubstrateState_None;
        if (a_actor == g_anchorId.load(std::memory_order_relaxed)) return APMF_API::kSubstrateState_Anchor;
        if (!AnyLive()) return APMF_API::kSubstrateState_None;
        std::uint32_t st = 0;
        if (FindSlot(a_actor, st) < 0 || !(st & kStLive)) return APMF_API::kSubstrateState_None;
        return (st & kStReal) ? APMF_API::kSubstrateState_Real : APMF_API::kSubstrateState_Held;
    }

    const char* RefuseReason(RE::FormID a_actor, const APMF_API::APMF_Param* a_param) {
        if (!Installed()) return NotInstalledReason();
        if (a_actor == 0x14) return "the actor is the player (the substrate serves NPC casts only)";
        if (a_actor == 0) return "actor 0";
        if (a_param && (std::isnan(a_param->fval) || a_param->fval < 0.0f))
            return "param.fval (the TTL in seconds) is negative or not a number (0 = the INI default)";
        return nullptr;
    }

    // A behaviour-tree node's name: the BSFixedString at node+0x08, written by the tree builders' one naming
    // helper (AE 0x85EE10 / SE 0x7C7820 / 1.7.104 0x874040: `lea rcx,[node+8]` + the BSFixedString copy). The
    // node is the act()'s own `this`, a static tree node alive for the session. ASCII case-insensitive compare
    // (the string pool keeps the first spelling it interned).
    bool NodeNamed(const void* a_node, const char* a_lit) {
        if (!a_node) return false;
        const char* n = reinterpret_cast<const RE::BSFixedString*>(static_cast<const std::uint8_t*>(a_node) + 0x08)->c_str();
        if (!n) return false;
        for (; *n && *a_lit; ++n, ++a_lit) {
            const char x = (*n >= 'A' && *n <= 'Z') ? static_cast<char>(*n - 'A' + 'a') : *n;
            const char y = (*a_lit >= 'A' && *a_lit <= 'Z') ? static_cast<char>(*a_lit - 'A' + 'a') : *a_lit;
            if (x != y) return false;
        }
        return *n == 0 && *a_lit == 0;
    }

    Leaf ForLeaf(RE::FormID a_actor, std::uint32_t a_leafIdx, bool /*a_movement*/, const void* a_node) {
        std::uint32_t st = 0;
        const int     si = FindSlot(a_actor, st);
        if (si < 0) return Leaf::kNotHolder;   // not a holder: nothing to count
        // fix/apmf-substrate-release-close (deny-hole probe): a HOLDER's early return is counted by the bit that failed,
        // the first one's leaf and node kept, for the per-window LEAF GATE line (emitted at the close)
        const std::uint32_t gw = a_leafIdx >= kLeafCount  ? kGateBadIdx
                               : !(st & kStLive)          ? kGateNotLive
                               : (st & kStReal)           ? kGateReal
                               : !(st & kStRankAction)    ? kGateNoRank
                                                          : 0u;
        if (gw != 0) {
            Slot& gs = g_slots[si];
            std::atomic<std::uint32_t>* ctr = gw == kGateBadIdx  ? &gs.gateBadIdx
                                            : gw == kGateNotLive ? &gs.gateNotLive
                                            : gw == kGateReal    ? &gs.gateReal
                                                                 : &gs.gateNoRank;
            ctr->fetch_add(1, std::memory_order_relaxed);
            std::uint32_t none = 0;
            if (gs.gateFirstWhy.compare_exchange_strong(none, gw, std::memory_order_relaxed)) {
                gs.gateFirstLeaf.store(a_leafIdx, std::memory_order_relaxed);
                gs.gateFirstNode.store(a_node, std::memory_order_relaxed);
                gs.gateFirstMs.store(apmf::clock::MonotonicMs(), std::memory_order_relaxed);
            }
            return Leaf::kNotHolder;
        }
        // fix/apmf-ooc-substrate: the TTL runs from the tree's first node under the substrate (Serve reads it).
        if (g_slots[si].treeStartMs.load(std::memory_order_relaxed) == 0) {
            std::uint64_t zero = 0;
            g_slots[si].treeStartMs.compare_exchange_strong(zero, apmf::clock::MonotonicMs(), std::memory_order_relaxed);
        }
        switch (g_leafClass[a_leafIdx]) {
        case LeafClass::kStructural:
            // THE MOVEMENT HOLD: the root of the Movement half runs held (core/ActionGate.cpp); the Combat Parallel's
            // entries are counted (one per window = it kept running; the old fail loop entered it every frame).
            if (a_leafIdx == g_parallelIdx) {
                if (NodeNamed(a_node, "Movement Parallel")) {
                    if (g_moveHold.load(std::memory_order_relaxed)) return Leaf::kHold;
                } else if (NodeNamed(a_node, "Combat Parallel")) {
                    g_slots[si].combatParallelActs.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return Leaf::kAllow;
        case LeafClass::kCast:
            if (st & kStCast) {
                g_slots[si].castAllowed.fetch_add(1, std::memory_order_relaxed);
                return Leaf::kAllow;
            }
            return Leaf::kDeny;   // nothing claimed: nothing is cast (the dry run)
        case LeafClass::kEquip:   // the spell-equip path: only for a driving claim, AcquireResource only "Action"
            if ((st & kStCast) && (st & kStEquipOk) && apmf::substrateequip::Allows(g_equipNode[a_leafIdx], a_node)) {
                apmf::substrateequip::NoteAllowed(si, a_actor, g_equipNode[a_leafIdx]);
                return Leaf::kAllow;
            }
            return Leaf::kDeny;
        default:
            return Leaf::kDeny;
        }
    }

    bool HoldsMovement(RE::FormID a_actor) {
        if (!g_moveHold.load(std::memory_order_relaxed) || !AnyLive()) return false;
        std::uint32_t st = 0;
        return FindSlot(a_actor, st) >= 0 && (st & kStLive) && !(st & kStReal) && (st & kStRankAction);
    }

    void NoteHold(RE::FormID a_actor, HoldEvent a_event) {
        std::uint32_t st = 0;
        const int     si = FindSlot(a_actor, st);
        if (si < 0) return;
        Slot& s = g_slots[si];
        switch (a_event) {
        case HoldEvent::kHeld:     s.holds.fetch_add(1, std::memory_order_relaxed); break;
        case HoldEvent::kUpdate:   s.holdUpdates.fetch_add(1, std::memory_order_relaxed); break;
        case HoldEvent::kReleased: s.holdReleased.fetch_add(1, std::memory_order_relaxed); break;
        case HoldEvent::kAnomaly:  s.holdAnomalies.fetch_add(1, std::memory_order_relaxed); break;
        }
    }

    void NoteClaimScore(RE::FormID a_actor, RE::FormID a_form, float a_score, const RE::CombatController* a_cc) {
        if (!AnyLive() || a_form == 0) return;
        std::uint32_t st = 0;
        const int     si = FindSlot(a_actor, st);
        if (si < 0 || !(st & kStLive) || (st & kStReal) || !(st & kStCast)) return;
        if (g_slots[si].claimScoreLogged.exchange(true, std::memory_order_relaxed)) return;   // one line per window
        // The spell score's inputs (AE CalculateScore 0x819AA0 -> 0x819C10, SE / 1.7.104 the same shape): the live
        // controller's combat style (CombatController+0x38, below the 0x68 AE split) times its CSGD magicScoreMult
        // (TESCombatStyle+0x30) times the CombatMagicItemData effect sum (x2 when the item's +0x34 is above 1024).
        // A score of 0 with magicScoreMult 0 is the actor's own combat style, not a substrate input.
        const RE::TESCombatStyle* cs = a_cc ? a_cc->combatStyle : nullptr;
        try {
            spdlog::info("[substrate] 0x{} CLAIM SCORE: the engine scored the claimed spell 0x{} at {:.3f} (combat style "
                         "0x{} magicScoreMult {:.3f}){}",
                         Hex(a_actor), Hex(a_form), static_cast<double>(a_score), Hex(cs ? cs->GetFormID() : 0),
                         static_cast<double>(cs ? cs->generalData.magicScoreMult : 0.0f),
                         a_score > 0.0f ? ""
                         : (cs && !(cs->generalData.magicScoreMult > 0.0f))
                             ? " -- 0 because this controller's combat style gives magic no weight (the actor's own style: "
                               "the substrate changes no score input). The engine then never selects it; see Docs/STATUS.md."
                             : " -- 0 or less from the spell's own effects against its target (true data).");
        } catch (...) {
        }
    }

    void LeafDenied(RE::FormID a_actor, std::uint32_t a_leafIdx, bool a_movement) {
        if (a_leafIdx >= kLeafCount) return;
        std::uint32_t st = 0;
        const int     si = FindSlot(a_actor, st);
        if (si < 0) return;
        Slot& s = g_slots[si];
        s.leafDenied.fetch_add(1, std::memory_order_relaxed);
        if (a_movement) s.moveDenied.fetch_add(1, std::memory_order_relaxed);
        const auto n = s.perLeaf[a_leafIdx].fetch_add(1, std::memory_order_relaxed) + 1;
        try {
            if (st & kStReal) {   // the trigger landed between the verdict and the deny
                s.afterReal.fetch_add(1, std::memory_order_relaxed);
                spdlog::warn("[substrate] 0x{} DENIED {} AFTER REAL COMBAT", Hex(a_actor), g_leafName[a_leafIdx]);
            } else if (n == 1) {
                spdlog::info("[substrate] 0x{} LEAF DENIED {} ({})", Hex(a_actor), g_leafName[a_leafIdx], n);
            }
        } catch (...) {
        }
    }

    bool RefuseAnchorTarget(RE::Actor* a_target, RE::Actor* a_caller) {
        if (!AnyLive() || !a_target) return false;
        const RE::FormID anchor = g_anchorId.load(std::memory_order_relaxed);
        if (anchor == 0 || a_target->GetFormID() != anchor) return false;
        const RE::FormID caller = a_caller ? a_caller->GetFormID() : 0;
        std::uint32_t    st     = 0;
        const int        si     = FindSlot(caller, st);
        // A holder entering (or re-adding) the anchor passes. So does a holder whose real-combat trigger fired
        // (design 3.6a: the refusal stands down for it on the combat thread at once); after the game-thread pass
        // drops it from the table it is refused like any other actor.
        if (si >= 0 && (st & kStLive)) return false;
        // Ranked at open: a substrate opens only while its client outranks Engine on Combat.
        const auto       n      = g_anchorRefusals.fetch_add(1, std::memory_order_relaxed) + 1;
        const RE::FormID served = g_served.load(std::memory_order_relaxed);
        for (auto& s : g_slots)
            if (s.id.load(std::memory_order_relaxed) == served) s.anchorRefused.fetch_add(1, std::memory_order_relaxed);
        try {
            const auto now  = apmf::clock::MonotonicMs();
            auto       prev = g_anchorRefusalLastLogMs.load(std::memory_order_relaxed);
            if (now - prev >= 2000 && g_anchorRefusalLastLogMs.compare_exchange_strong(prev, now)) {
                spdlog::info("[substrate] 0x{} STARTCOMBAT-AT-ANCHOR REFUSED caller=0x{} ({} so far)", Hex(served),
                             Hex(caller), n);
            }
        } catch (...) {
        }
        return true;
    }

    void RemoveAnchorFromGroup(RE::CombatGroup* a_g) {
        auto* anchor = Anchor();
        if (a_g && anchor) g_removeTarget(a_g, anchor);
    }

    bool IsAnchorRef(const RE::TESObjectREFR* a_target) {
        const auto a = g_anchorId.load(std::memory_order_relaxed);
        return a_target && a != 0 && a_target->GetFormID() == a;
    }

    bool MusicDebtPending() { return g_debtCount.load(std::memory_order_acquire) != 0; }

    void NoteGroupTarget(void* a_group, RE::TESObjectREFR* a_target) {
        if (!a_group || !a_target) return;
        SeatMusicRealTarget(static_cast<RE::CombatGroup*>(a_group), a_target);   // before the drop can clear any slot
        apmf::substratebark::SeatRealTarget(static_cast<RE::CombatGroup*>(a_group),
                                            WriteLockedHere(static_cast<RE::CombatGroup*>(a_group)));   // R5 F-2 put-back
        if (!AnyLive()) return;
        auto*               g       = static_cast<RE::CombatGroup*>(a_group);
        const std::uint32_t anchorH = g_anchorH.load(std::memory_order_acquire);
        bool                other   = false;
        bool                looked  = false;
        for (int i = 0; i < kSlots; ++i) {
            const RE::FormID id = g_slots[i].id.load(std::memory_order_acquire);
            if (id == 0) continue;
            const auto st = g_slots[i].state.load(std::memory_order_acquire);
            if ((st & (kStLive | kStReal)) != kStLive) continue;
            if (!GroupHasMember(g, g_slots[i].holderH.load(std::memory_order_relaxed))) continue;
            if (!looked) {
                other  = FirstNonAnchorTarget(g, anchorH) != nullptr;   // a real target in the holder's group
                looked = true;
            }
            if (!other) return;
            MarkReal(i, kWhyAddedTarget, a_target->GetFormID());
        }
    }

    void AnchorKnowledge(void* a_group, RE::TESObjectREFR* a_anchor) {
        if (!AnyLive() || !a_group || !a_anchor) return;
        auto* g = static_cast<RE::CombatGroup*>(a_group);
        SeatMusicAnchor(g);   // the music deny / probe: inside the engine's own locked update only
        apmf::substratebark::SeatAnchorPass(g, WriteLockedHere(g));   // R5 F-2 / F-7: the two dialogue timers read
        for (int i = 0; i < kSlots; ++i) {
            if (g_slots[i].id.load(std::memory_order_acquire) == 0) continue;
            const auto st = g_slots[i].state.load(std::memory_order_acquire);
            if ((st & (kStLive | kStReal)) != kStLive || !(st & kStRankDetect)) continue;
            if (!GroupHasMember(g, g_slots[i].holderH.load(std::memory_order_relaxed))) continue;
            RE::CombatTarget* t = FindTargetEntry(g, g_anchorH.load(std::memory_order_acquire));
            if (!t) return;
            const std::uint16_t fl = t->flags.underlying();
            if (fl & kTargetLost) {   // the engine's own un-lose (AE 0x805483), as the held engagement writes it
                t->lastKnownTimeStamp.timeStamp = (std::max)(t->lastKnownTimeStamp.timeStamp, t->unk94.timeStamp);
                t->flags              = static_cast<RE::CombatTarget::Flags>(fl & ~kTargetLost);
                t->unk94.timeStamp    = -(std::numeric_limits<float>::max)();
                g_slots[i].unlost.fetch_add(1, std::memory_order_relaxed);
            }
            if (const void* space = apmf::combatapproach::SpaceOfRef(a_anchor)) {
                t->lastKnownLoc.pos              = a_anchor->GetPosition();
                t->lastKnownLoc.cellOrWorldSpace = const_cast<RE::TESForm*>(static_cast<const RE::TESForm*>(space));
            }
            return;   // one write per event: the entry is the group's
        }
    }

    void NoteCastFired(RE::CombatController* a_cc, RE::CombatMagicCaster* a_caster) {
        if (!AnyLive() || !a_cc || !a_caster || !a_caster->magicItem) return;
        auto        att = a_cc->attackerHandle.get();
        const auto* a   = att.get();
        if (!a) return;
        std::uint32_t st = 0;
        const int     si = FindSlot(a->GetFormID(), st);
        if (si < 0) return;
        g_slots[si].fires.fetch_add(1, std::memory_order_relaxed);
        g_slots[si].firedForm.store(a_caster->magicItem->GetFormID(), std::memory_order_release);
    }

    void NoteCastBuilt(RE::CombatController* a_cc, RE::CombatMagicCaster* a_caster) {
        if (!AnyLive() || !a_cc || !a_caster || !a_caster->magicItem) return;
        auto        att = a_cc->attackerHandle.get();
        const auto* a   = att.get();
        if (!a) return;
        std::uint32_t st = 0;
        const int     si = FindSlot(a->GetFormID(), st);
        if (si < 0 || (st & (kStLive | kStReal)) != kStLive) return;
        const std::uint64_t treeMs = g_slots[si].treeStartMs.load(std::memory_order_relaxed);
        // PASSIVE PROBE (fix/apmf-gate-probe-proxy-name, 2026-10-26; principle 5): the FIRST build per window, inside
        // or outside the gate, says WHICH controller built it -- the engine's `a_cc`, its attacker (the holder: that is
        // how the slot was found) and whether `a_cc` is the holder's own live combatController (via GetActorRuntimeData: SE +0x158 / AE and 1.7 +0x160; a pointer
        // COMPARE only: never dereferenced here). A build on a controller that is not the holder's would explain a tree
        // whose control resolved to another id. Writes nothing the gate or the close reads.
        if (!g_slots[si].builtProbeLogged.exchange(true, std::memory_order_relaxed)) {
            const RE::CombatController* live = a->GetActorRuntimeData().combatController;
            spdlog::info("[substrate] CAST BUILT 0x{}: spell 0x{} on controller 0x{} (attacker 0x{}, handle 0x{}); the "
                         "holder's live combatController 0x{} -- {}; {}; thread {} (first build per window only).",
                         Hex(a->GetFormID()), Hex(a_caster->magicItem->GetFormID()),
                         apmf::log::Hex(reinterpret_cast<std::uintptr_t>(a_cc), 16), Hex(a->GetFormID()),
                         Hex(a_cc->attackerHandle.native_handle()),
                         apmf::log::Hex(reinterpret_cast<std::uintptr_t>(live), 16),
                         live == a_cc ? "the SAME controller" : "a DIFFERENT controller",
                         treeMs != 0 ? "inside the gate (the leaf gate saw this holder's tree)"
                                     : "OUTSIDE the gate (treeStartMs 0)",
                         REX::W32::GetCurrentThreadId());
        }
        // The tree's leaves run ForLeaf (which sets treeStartMs) before a cast leaf's context builds a caster, on this
        // same combat thread: a build with treeStartMs still 0 ran outside the gate. The first one is kept.
        if (treeMs != 0) return;
        RE::FormID zero = 0;
        if (g_slots[si].builtOutside.compare_exchange_strong(zero, a_caster->magicItem->GetFormID(),
                                                             std::memory_order_acq_rel))
            g_slots[si].builtOutsideMs.store(apmf::clock::MonotonicMs(), std::memory_order_relaxed);
    }

    void Install() {
        if (g_installTried.exchange(true)) return;
        const char* why = nullptr;
        if (IniInt("CombatSubstrate", "bCombatSubstrate", 1) == 0) {
            why = "[CombatSubstrate] bCombatSubstrate=0 (set in APMF.ini: the substrate is switched off)";
        } else if (REL::Module::IsVR() || !apmf::allowance::RuntimeSupported()) {
            why = "the runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104";
        } else if (!apmf::reentrydeny::Installed()) {
            why = "the ch.22 slot-0x99 seat is not installed (the anchor refusal rides it; [CombatReentryDeny] "
                  "bCombatReentryDeny=0 refuses the substrate too)";
        } else if (!apmf::reentrydeny::TargetSiteInstalled()) {
            why = "the anchor-target refusal is not installed on ch.22's seat (its call-site row or the rdi stub)";
        } else if (!apmf::combatentry::hold::StopCombatAvailable()) {
            why = "the held engagement's ranked StopCombat is not available ([EngagementHold] bEngagementHold=0, a "
                  "runtime other than exactly 1.6.1170 / 1.5.97 / 1.7.104, or its Character vtable row refused): the real-combat "
                  "drop ends an EMPTY fight through it (marth 2026-10-13), so the substrate needs it";
        } else if (apmf::actiongate::SubstrateLeavesPaired() != kLeafCount) {
            why = "the leaf allow-list does not cover all 95 behaviour-tree node classes with an act()/pop() pair and the "
                  "ForceFail pair (the 70-leaf catalog, the four search leaves ch.23 installs -- [PursuitLeash] "
                  "bPursuitLeash=0 refuses the substrate too -- and the 21 parameterised nodes ActionGate installs when "
                  "bCombatSubstrate=1)";
        }
        const bool on17 = apmf::allowance::IsRuntime1_7_104();   // raw-RVA rows for the id-less addresses (above)
        if (!why) {
            bool ok = apmf::allowance::SeatVerified(
                REL::Relocation<std::uintptr_t>{ RELOCATION_ID(37608, 38561) }.address(), "CombatEntry.Actor.StartCombat");
            ok = (on17 ? apmf::allowance::SeatVerified(REL::Offset(k17RemoveTarget).address(),
                                                       "Substrate.CombatGroup.RemoveTarget.1_7_104")
                       : apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(43483, 44705) }.address(),
                                                       "Substrate.CombatGroup.RemoveTarget")) && ok;
            ok = apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(56227, 56626) }.address(),
                                               "Substrate.TESObjectREFR.MoveTo") && ok;
            ok = apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(36407, 37401) }.address(),
                                               "PackageGate.EvaluatePackage") && ok;
            const REL::Relocation<std::uintptr_t> vtChar{ RE::VTABLE_Character[0] };
            const REL::Relocation<std::uintptr_t> vtPc{ RE::VTABLE_PlayerCharacter[0] };
            ok = apmf::allowance::SeatVerified(vtChar.address(), "Character (0xE1 SetAlpha, 0xE5 StopCombat)") && ok;
            ok = apmf::allowance::SeatVerified(vtPc.address(), "PlayerCharacter (0xE3 IsInCombat)") && ok;
            ok = apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(14108, 14298) }.address(),
                                               "Travel.ScriptEventSourceHolder.GetSingleton (the hit / combat sinks)") && ok;
            if (!ok) why = "the address self-check refused a row this path calls (listed above)";
            g_vtChar = vtChar.address();
        }
        // The music deny (ClickUp 86e3pu939; seat form after review F6 of b1c78b2). Every engine fact it rests on is a
        // verified row: the group update takes the WRITE lock at +0x160 first (MusicWriteLock), calls the group detection
        // pass the seat rides (DetectionPassInUpdate), decides +0x15C only while it is 0xFF (MusicUndecided), and the
        // should-play test reads +0x15C == 1 (MusicPlayTest); the lock rows (LockForWrite / UnlockForWrite) pin the
        // +0 writer-thread / +4 count layout WriteLockedHere reads (Harbinger never calls them for music). The seat is
        // the held engagement's knowledge seat (combatentry::hold::KnowledgeInstalled). A failed row or a missing seat
        // REFUSES THE SUBSTRATE (principle 2: it would switch battle music on undenied);
        // [CombatSubstrate] bSubstrateMusicDeny=0 is the explicit off switch (the substrate runs, every OPEN logs the
        // hole). 1.7.104: the two engine functions have no MIT id-table row (a RELOCATION_ID miss is fatal there), so the
        // four sites are raw-RVA rows plus the byte / call-target re-reads above (k17*; the AE offsets, bodies identical).
        if (!why) {
            if (IniInt("CombatSubstrate", "bSubstrateMusicDeny", 1) == 0) {
                g_musicDeny    = false;
                g_musicDenyOff = "[CombatSubstrate] bSubstrateMusicDeny=0 in APMF.ini";
            } else if (!apmf::allowance::RuntimeSupported()) {
                why = "the music deny's engine functions are verified on exactly 1.6.1170, 1.5.97 and 1.7.104 only";
            } else {
                const auto lockW = REL::Relocation<std::uintptr_t>{ RE::Offset::BSReadWriteLock::LockForWrite }.address();
                bool       ok    = apmf::allowance::SeatVerified(lockW, "CommonLib.BSReadWriteLock.LockForWrite");
                ok = apmf::allowance::SeatVerified(
                         REL::Relocation<std::uintptr_t>{ RE::Offset::BSReadWriteLock::UnlockForWrite }.address(),
                         "CommonLib.BSReadWriteLock.UnlockForWrite") && ok;
                if (on17) {
                    static constexpr std::array<std::uint8_t, 7> kUndecided{ 0x80, 0xBF, 0x5C, 0x01, 0x00, 0x00, 0xFF };
                    static constexpr std::array<std::uint8_t, 7> kPlayTest{ 0x80, 0xBD, 0x5C, 0x01, 0x00, 0x00, 0x01 };
                    const auto wl = REL::Offset(k17MusicWriteLock).address();
                    const auto dp = REL::Offset(k17DetectionPassInUpdate).address();
                    const auto mu = REL::Offset(k17MusicUndecided).address();
                    const auto pt = REL::Offset(k17MusicPlayTest).address();
                    ok = apmf::allowance::SeatVerified(wl, "Substrate.CombatGroup.MusicWriteLock.1_7_104") &&
                         CallsTo17(wl, lockW, "Substrate.CombatGroup.MusicWriteLock.1_7_104") && ok;
                    ok = apmf::allowance::SeatVerified(dp, "Substrate.CombatGroup.DetectionPassInUpdate.1_7_104") &&
                         CallsTo17(dp, REL::Offset(k17DetectionPass).address(),
                                   "Substrate.CombatGroup.DetectionPassInUpdate.1_7_104") && ok;
                    ok = apmf::allowance::SeatVerified(mu, "Substrate.CombatGroup.MusicUndecided.1_7_104") &&
                         BytesAt17(mu, kUndecided, "Substrate.CombatGroup.MusicUndecided.1_7_104") && ok;
                    ok = apmf::allowance::SeatVerified(pt, "Substrate.CombatManager.MusicPlayTest.1_7_104") &&
                         BytesAt17(pt, kPlayTest, "Substrate.CombatManager.MusicPlayTest.1_7_104") && ok;
                } else {
                    const bool ae  = REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170);
                    const auto upd = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(43481, 44703) }.address();
                    const auto spt = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(45588, 46888) }.address();
                    ok = apmf::allowance::SeatVerified(upd + (ae ? 0x30 : 0x2B), "Substrate.CombatGroup.MusicWriteLock") && ok;
                    ok = apmf::allowance::SeatVerified(upd + (ae ? 0x6D : 0x65), "Substrate.CombatGroup.DetectionPassInUpdate") && ok;
                    ok = apmf::allowance::SeatVerified(upd + (ae ? 0x23C : 0x94), "Substrate.CombatGroup.MusicUndecided") && ok;
                    ok = apmf::allowance::SeatVerified(spt + (ae ? 0x1E9 : 0x1E7), "Substrate.CombatManager.MusicPlayTest") && ok;
                }
                if (ok && !apmf::combatentry::hold::KnowledgeInstalled()) {
                    why = "the music deny rides the held engagement's knowledge seat (the group detection pass inside the "
                          "engine's locked group update), which is not installed (see the [hold] knowledge seats line)";
                } else if (ok) {
                    g_musicDeny    = true;
                    g_musicDenyOff = "";
                } else {
                    why = "the address self-check refused a row the music deny rests on (listed above): without it the "
                          "substrate would switch battle music on undenied";
                }
            }
        }
        // R5 (marth 2026-10-19 rulings Q1 / Q2): the combat-start bark deny (F-2) and the aggressor-bit clear (F-3). Each
        // is a deny of a facet the substrate switches on: a row that does not verify REFUSES THE SUBSTRATE (principle 2),
        // as the music deny does; its INI switch turns it off with the hole logged per OPEN. 1.7.104: refused by name.
        if (!why) why = apmf::substratebark::Install();
        if (!why) why = apmf::substrateaggressor::Install();
        if (!why) why = apmf::substratepackage::Install();   // F-6 review: a refused override seat refuses the substrate
        // fix/apmf-ooc-substrate: THE MOVEMENT HOLD (core/ActionGate.cpp). Without it the Movement half's force-failed
        // leaves fail the Combat Parallel every frame and its Action half (the spell equip, the cast leaves) never runs:
        // a substrate would open onto a cast it can never reach. A hold that did not arm REFUSES THE SUBSTRATE (never a
        // silent dead window); [CombatSubstrate] bSubstrateMovementHold=0 is the explicit off switch (the substrate
        // runs as before, and every OPEN logs the hole).
        if (!why) {
            for (std::uint32_t i = 0; i < apmf::cbt::kLeaves.size(); ++i)
                if (std::string_view(apmf::cbt::kLeaves[i].name) == "CombatBehaviorParallel") g_parallelIdx = i;
            if (IniInt("CombatSubstrate", "bSubstrateMovementHold", 1) == 0) {
                g_moveHold.store(false, std::memory_order_relaxed);
                g_moveHoldOff = "[CombatSubstrate] bSubstrateMovementHold=0 in APMF.ini";
            } else if (g_parallelIdx == 0xFFFFFFFFu) {
                why = "the leaf catalog has no CombatBehaviorParallel (the movement hold's node)";
            } else if (!apmf::actiongate::MovementHoldArmed()) {
                why = "the movement hold did not arm (see the [substrate] MOVEMENT HOLD NOT armed line): without it the "
                      "Movement half fails the Combat Parallel and no substrate cast is reachable";
            } else {
                g_moveHold.store(true, std::memory_order_relaxed);
                g_moveHoldOff = "";
            }
        }
        RE::TESNPC*    npc  = nullptr;
        RE::Actor*     ref  = nullptr;
        if (!why) {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                why = "TESDataHandler is unavailable at kDataLoaded";
            } else {
                npc     = dh->LookupForm<RE::TESNPC>(kLocalAnchorNpc, kPlugin);
                ref     = dh->LookupForm<RE::Actor>(kLocalAnchorRef, kPlugin);
                g_homeId = dh->LookupFormID(kLocalAnchorHome, kPlugin);
                g_cellId = dh->LookupFormID(kLocalHoldingCell, kPlugin);
                if (!npc)                                why = "APMF.esl's anchor NPC_ (0x808) does not resolve -- is the plugin current?";
                else if (!ref)                           why = "APMF.esl's anchor ACHR (0x80A) does not resolve as an Actor";
                else if (ref->GetActorBase() != npc)     why = "APMF.esl's 0x80A is not an instance of 0x808";
                else if (!g_homeId || !g_cellId)         why = "APMF.esl's holding cell (0x809) or home marker (0x80B) does not resolve";
            }
        }
        if (why) {
            g_notInstalledReason.store(why, std::memory_order_release);
            spdlog::warn("[substrate] combat substrate NOT installed -- {}. kIntent_CombatSubstrate claims are REFUSED "
                         "and the dry run does not run.",
                         why);
            return;
        }
        g_anchorId.store(ref->GetFormID(), std::memory_order_release);
        g_removeTarget    = on17 ? REL::Relocation<RemoveTarget_t>{ REL::Offset(k17RemoveTarget) }
                                 : REL::Relocation<RemoveTarget_t>{ RELOCATION_ID(43483, 44705) };
        g_evaluatePackage = REL::Relocation<EvaluatePkg_t>{ RELOCATION_ID(36407, 37401) };
        // The two singletons resolve through the MIT id table on 1.7.104 (ids 400896 / 401099 are rows of it); their
        // 1.7.104 proof is the reader that reads them (the 1.6.1170 / 1.5.97 ripref rows, re-done here in code).
        const auto musicSingleton = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(514738, 400896) }.address();
        g_musicOk = on17 ? ReaderReads17(k17MusicReader, { 0x48, 0x8B, 0x0D }, { 0x48, 0x8D, 0x54, 0x24, 0x30, 0x48, 0x89, 0x74 },
                                         musicSingleton, "Substrate.BSMusicManager.Reader.1_7_104")
                         : apmf::allowance::SeatVerified(musicSingleton, "Substrate.BSMusicManager.Singleton");
        // The dry run's dialogue test (MenuTopicManager::speaker, +0x68 on all three: IsInDialogueWithPlayer is identical
        // on 1.7.104). On a failed row every teammate counts as "dialogue unknown -> not idle" and the dry run opens nothing.
        const auto topicSingleton = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(514959, 401099) }.address();
        g_dialogueOk = on17 ? ReaderReads17(k17MenuTopicReader, { 0x48, 0x8B, 0x05 },
                                            { 0x48, 0x8D, 0x54, 0x24, 0x40, 0x48, 0x89, 0x5C }, topicSingleton,
                                            "Substrate.MenuTopicManager.Reader.1_7_104")
                            : apmf::allowance::SeatVerified(topicSingleton, "Substrate.MenuTopicManager.Singleton");
        if (!g_dialogueOk)
            spdlog::warn("[substrate] the MenuTopicManager row did not verify on this runtime: the dry run cannot tell whether a "
                         "follower is in dialogue with the player, so it treats EVERY teammate as not idle and opens nothing.");
        g_fireSignal = apmf::castertypecensus::NotifyInstalled();

        // the leaf classes (design 3.5's allow-list)
        constexpr std::size_t kL = apmf::cbt::kLeaves.size(), kS = apmf::cbt::kSearchLeaves.size();
        for (std::uint32_t i = 0; i < kLeafCount; ++i) {
            if (i >= kL + kS) {   // a parameterised node: its class is in the catalog (review F1)
                const auto& e = apmf::cbt::kParamLeaves[i - kL - kS];
                g_leafName[i]  = e.name;
                g_leafClass[i] = e.kind == apmf::cbt::kParamCast       ? LeafClass::kCast
                               : e.kind == apmf::cbt::kParamStructural ? LeafClass::kStructural
                                                                       : LeafClass::kDeny;
                continue;
            }
            const char* nm = i < kL ? apmf::cbt::kLeaves[i].name : apmf::cbt::kSearchLeaves[i - kL].name;
            g_leafName[i] = nm;
            const std::string_view n = nm;
            if (n == "CombatBehaviorCastImmediateSpell" || n == "CombatBehaviorCastConcentrationSpell" ||
                n == "CombatBehaviorPrepareDualCast")
                g_leafClass[i] = LeafClass::kCast;
            else if (n.starts_with("CombatBehaviorChildSelector_") || n.starts_with("CombatBehaviorFallbackSelector_") ||
                     n == "CombatBehaviorDynamicConditionalNode" || n == "CombatBehaviorParallel" ||
                     n == "CombatBehaviorSequence" || n == "CombatBehaviorRepeat" || n == "CombatBehaviorForceFail" ||
                     n == "CombatBehaviorForceSuccess")
                g_leafClass[i] = LeafClass::kStructural;
            else
                g_leafClass[i] = LeafClass::kDeny;
        }
        // the spell-equip path (channels/SubstrateEquip.cpp): EquipSpell + AcquireResource<2> ("Action" only)
        int equipNodes = 0;
        for (std::uint32_t i = 0; i < kLeafCount; ++i) {
            const auto en = apmf::substrateequip::Classify(g_leafName[i] ? g_leafName[i] : "");
            g_equipNode[i] = en;
            if (en != apmf::substrateequip::Node::kNone) {
                g_leafClass[i] = LeafClass::kEquip;
                ++equipNodes;
            }
        }
        if (apmf::substrateequip::Enabled() && equipNodes != 2)
            spdlog::error("[substrate] the spell-equip path found {} of its 2 nodes in the catalog: a substrate cast "
                          "cannot reach the spell equip.", equipNodes);

        g_dryRun       = IniInt("CombatSubstrate", "bSubstrateDryRun", 0) != 0;
        g_ttlSec       = std::clamp(IniFloat("CombatSubstrate", "fSubstrateTtlSec", 6.0f), 1.0f, kMaxTtlSec);
        g_dryPeriodSec = std::max(5.0f, IniFloat("CombatSubstrate", "fSubstrateDryRunPeriodSec", 20.0f));
        g_moveTrace    = IniInt("CombatSubstrate", "bSubstrateMoveTrace", 0) != 0;
        g_census       = IniInt("CombatSubstrate", "bSubstrateBystanderCensus", 1) != 0;
        apmf::substrateprobe::Install(g_dialogueOk);   // R1 probes ([CombatSubstrate] bSubstrateProbes)

        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESHitEvent>(HitSink::Get());
            holder->AddEventSink<RE::TESCombatEvent>(CombatEventSink::Get());
            holder->AddEventSink<RE::TESSpellCastEvent>(SpellCastSink::Get());   // the release evidence (APMF-B93)
            apmf::substrateprobe::CombatEventSinkOrder(holder, CombatEventSink::Get());   // R5 F-4: the passive probe only
        } else {
            spdlog::error("[substrate] ScriptEventSourceHolder unavailable: trigger (b) (a hit on the holder), the "
                          "combat-state-event log and the TESSpellCastEvent release evidence are OFF (no window can close "
                          "as fired: released-unconfirmed or fired-not-released only); triggers (a) and (c) still drop a substrate on "
                          "real combat.");
        }
        g_notInstalledReason.store("", std::memory_order_release);
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[substrate] combat substrate INSTALLED: anchor 0x{} (APMF.esl 0x80A, a {} clone), home 0x{}; dry run {} "
                     "(period {:.0f} s), TTL {:.1f} s, P6 move trace {}, fire signal {}, music {}, music deny {}, bark deny {}, "
                     "aggressor clear {}, movement hold {}, tree-start floor {} ms (the TTL runs from the tree's start). "
                     "Allow-list: "
                     "3 cast leaves, the spell-equip path {} (EquipSpell + AcquireResource<2> \"Action\", while a "
                     "driving cast claim stands), {} structural nodes pass, the rest denied.",
                     Hex(ref->GetFormID()), npc->GetName(), Hex(g_homeId), g_dryRun ? "ON" : "off",
                     static_cast<double>(g_dryPeriodSec), static_cast<double>(g_ttlSec), g_moveTrace ? "ON" : "off",
                     g_fireSignal ? "census 0x0B" : "NONE", g_musicOk ? "verified" : "n/a",
                     g_musicDeny ? std::string("ON") : fmt::format("OFF ({})", g_musicDenyOff),
                     apmf::substratebark::On() ? std::string("ON") : fmt::format("OFF ({})", apmf::substratebark::OffReason()),
                     apmf::substrateaggressor::On() ? std::string("ON")
                                                    : fmt::format("OFF ({})", apmf::substrateaggressor::OffReason()),
                     g_moveHold.load(std::memory_order_relaxed) ? std::string("ON") : fmt::format("OFF ({})", g_moveHoldOff),
                     kTreeStartWaitMs,
                     apmf::substrateequip::Enabled() ? "ON" : "OFF",
                     std::count(g_leafClass.begin(), g_leafClass.end(), LeafClass::kStructural));
    }

    void Poll() {
        if (!Installed()) return;
        ++g_pollFrame;
        const auto now = apmf::clock::MonotonicMs();
        DryRunSchedule(now);
        apmf::substrateprobe::Poll(now);   // R1: before the early return (the post-close timers run with nothing open)
        apmf::substrateequip::PollHands(now);   // the stowed hands' restore runs after the close
        DebtExpire(now);
        apmf::substratebark::Poll(now);   // R5 F-2: a dropped holder's bark put-back, paid or expired
        if (g_subCount.load(std::memory_order_relaxed) == 0 && g_subs.empty()) {
            MusicProbe(now);   // the tail after the last close
            apmf::anchorguard::Sweep(now);   // the anchor-leak sweep's tail (SEV-1 2026-10-19)
            BystanderCensus(now);
            if (g_dryRun) Heartbeat(now);
            return;
        }
        PartyPoll(now);
        // The winning handle of a client substrate, now that Drain published it.
        for (auto it = g_subs.begin(); it != g_subs.end();) {
            Sub& s = it->second;
            if (!s.dryRun && !s.closeAsked && s.handle == APMF_API::kInvalidHandle) {
                APMF_API::APMF_Param p{};
                float                b = 0.0f;
                APMF_API::Handle     h = APMF_API::kInvalidHandle;
                if (apmf::ControlMap::Get().TryGetOwningClaimBasis(s.id, APMF_API::kIntent_CombatSubstrate, p, b, &h))
                    s.handle = h;
            }
            bool keep = true;
            if (s.closeAsked && s.phase != Phase::kOpen) {
                if (s.slot >= 0) CloseSub(s, s.closeWhy ? s.closeWhy : "client release");
                keep = false;
            } else {
                switch (s.phase) {
                case Phase::kRequested:    keep = Begin(s); break;
                case Phase::kFloorsQueued: keep = Enter(s); break;
                case Phase::kOpen:         keep = Serve(s, now); break;
                }
            }
            it = keep ? std::next(it) : g_subs.erase(it);
        }
        g_subCount.store(g_subs.size(), std::memory_order_relaxed);
        PlaceAnchor();
        MusicProbe(now);
        apmf::anchorguard::Sweep(now);   // the anchor-leak sweep + the join guard's group set (SEV-1 2026-10-19)
        BystanderCensus(now);
        Heartbeat(now);
    }

    void ResetAll(const char* a_why) {
        // review F1 of df5635a: no hold record crosses the load / revert / new-game boundary (kPreLoadGame and the
        // revert, plugin.cpp): a record left for the next world could name a reused {thread, node} of a NATIVE
        // Parallel. AfterLoad forgets them again after the load.
        apmf::actiongate::ForgetMovementHolds(0);
        const std::size_t n = g_subs.size();
        g_subs.clear();
        for (int i = 0; i < kSlots; ++i) {
            g_slots[i].state.store(0, std::memory_order_release);
            g_slots[i].id.store(0, std::memory_order_release);
        }
        g_slotCount.store(0, std::memory_order_relaxed);
        g_subCount.store(0, std::memory_order_relaxed);
        g_anchorOut = 0;
        g_served    = 0;
        g_musicTracking = false;
        g_censusLive    = false;
        g_censusRows.clear();
        apmf::anchorguard::ResetAll();
        apmf::substrateprobe::Reset();
        apmf::substratepackage::Reset();
        apmf::substratebark::Reset();
        apmf::substrateaggressor::Reset();
        apmf::substrateequip::Reset();
        apmf::substrateequip::ResetHands(a_why);
        {
            std::scoped_lock lk(g_debtMx);
            g_debts.fill(MusicDebt{});
            DebtCountLocked();
        }
        if (n) spdlog::info("[substrate] {} -- forgot {} substrate(s); no engine call (the outgoing world's controllers "
                            "go with it).", a_why, n);
    }

    void AfterLoad(const char* a_why) {
        if (!Installed()) return;
        // fix/apmf-ooc-substrate: the outgoing world's behaviour threads are gone; a held node the save carried is an
        // ordinary empty Parallel now (its own SaveGame/LoadGame), so no hold record survives a load.
        apmf::actiongate::ForgetMovementHolds(0);
        auto* anchor = Anchor();
        if (!anchor) return;
        const auto anchorH = anchor->GetHandle().native_handle();
        g_anchorH.store(anchorH, std::memory_order_release);
        // A save written while a substrate was open: the anchor's position and its holders' controllers.
        std::uint32_t removed = 0, stopped = 0;
        if (auto* pl = RE::ProcessLists::GetSingleton()) {
            std::vector<RE::Actor*> withAnchor;
            pl->ForEachHighActor([&](RE::Actor* a) {
                auto* cc = a ? a->GetActorRuntimeData().combatController : nullptr;
                auto* g  = cc ? cc->combatGroup : nullptr;
                if (!g) return RE::BSContainer::ForEachResult::kContinue;
                bool has = false;
                {
                    RE::BSReadLockGuard guard(g->lock);
                    has = FindTargetEntry(g, anchorH) != nullptr;
                }
                if (has) withAnchor.push_back(a);
                return RE::BSContainer::ForEachResult::kContinue;
            });
            for (auto* a : withAnchor) {
                auto* cc = a->GetActorRuntimeData().combatController;
                auto* g  = cc ? cc->combatGroup : nullptr;
                if (!g) continue;
                g_removeTarget(g, anchor);
                ++removed;
                std::uint32_t left = 0;
                {
                    RE::BSReadLockGuard guard(g->lock);
                    left = g->targets.size();
                }
                if (left == 0 && *reinterpret_cast<const std::uintptr_t*>(a) == g_vtChar) {
                    a->StopCombat();
                    ++stopped;
                } else if (left != 0 && g_musicDeny) {
                    // review F3: +0x15C is SAVED (AE save 0x80ABE0 -> WriteData 0x80B28B; load 0x80B2D0 -> ReadData
                    // 0x80BB96). A group saved mid-substrate that also holds a real target may carry Harbinger's 0: this
                    // actor owes it back, and the seat puts it back inside the group's next locked update (it reads 0
                    // and shows a real target). A pure-substrate group (left == 0 above) is ended instead.
                    DebtSet(a->GetHandle().native_handle(), a->GetFormID(), apmf::clock::MonotonicMs() + kDebtAfterDropMs);
                }
                spdlog::warn("[substrate] {}: 0x{} came out of the save in combat with the anchor (a save written while a "
                             "substrate was open) -- anchor removed from its group{}.",
                             a_why, Hex(a->GetFormID()), left == 0 ? "; nothing else to fight: combat ended (StopCombat)" : "");
            }
        }
        if (auto* home = g_homeId ? RE::TESForm::LookupByID<RE::TESObjectREFR>(g_homeId) : nullptr;
            home && anchor->GetParentCell() != home->GetParentCell())
            AnchorHome(anchor, a_why);
        if (removed || stopped)
            spdlog::info("[substrate] {}: anchor cleaned from {} group(s), {} combat(s) ended.", a_why, removed, stopped);
    }

}

APMF_REGISTER_CHANNEL(CombatSubstrateChannel);
