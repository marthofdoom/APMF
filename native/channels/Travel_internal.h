#pragma once

// ============================================================================
// Channel 19 -- TRAVEL: the state and helpers the channel's files share.
// INTERNAL to native/channels/Travel*.cpp; nothing outside ch.19 includes it (the
// public surface is channels/Travel.h). Split out of the one Travel.cpp (backlog
// APMF-B33) as a pure move: every definition below is the original text, the
// variables are DEFINED once in the .cpp named beside them, and the threading
// notes in Travel.cpp's header still govern every one of them.
// ============================================================================

#include "APMF_API.h"
#include "core/Log.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

    using apmf::log::Hex;

    // ---- Shipped plugin + its FROZEN FormID band (APMF_GenerateESL.py) ----
    constexpr const char* kPlugin        = "APMF.esl";
    constexpr RE::FormID  kTravelPkgBase = 0x800;   // APMF_TravelPackage0
    // One package record per CONCURRENT leg. The Location input lives on the
    // RECORD, not on the actor, so two actors walking to two different places need
    // two records -- the same reason MFO ships four loot-travel records. Eight is
    // generous for a party-scale client and costs 8 PACK records in a 2.5 KB
    // plugin; past it the leg is REFUSED and logged loudly (never silently shared,
    // which would send both actors to one destination).
    constexpr std::size_t kTravelSlots = 8;

    // THE ARRIVAL RADIUS. marth's contract is "it goes to the target 50-100u from
    // it", so the default is the middle of that band. A client may pass its own in
    // `param.fval`; it is CLAMPED to [kMinRadiusUnits, kMaxRadiusUnits] and the
    // clamp is LOGGED, never applied silently.
    //
    // The same number is written into the package record's own Location radius for
    // this leg (core/PackageData.cpp SetTravelTarget writes `loc->rad`), so the
    // engine's own package stop and APMF's arrival test fire at the same distance
    // and cannot drift apart. The record's AUTHORED radius is only a placeholder --
    // it is overwritten before the package is ever offered, and if that write is
    // declined the leg is refused outright rather than offered with a stale one.
    // **The DLL's own test is the AUTHORITY** for ending the leg: it is what
    // releases the ch.9 claim; the record's radius only decides where the engine
    // parks the actor.
    //
    // Bounds: below ~50u an actor cannot reliably reach the point (bodies,
    // furniture and the destination's own collision), and above 512u "arrived"
    // stops meaning anything for the hand-off this exists to make.
    constexpr float kDefaultRadiusUnits = 75.0f;
    constexpr float kMinRadiusUnits     = 50.0f;
    constexpr float kMaxRadiusUnits     = 512.0f;

    // How often the monitor evaluates. It does one distance compare and one virtual
    // `IsInCombat()` per live leg -- at most kTravelSlots of each, four times a
    // second. Far finer than a travel leg needs, and cheap.
    constexpr std::uint64_t kPollPeriodMs = 250;

    // A SAFETY NET, not a feature budget (CLAUDE.md principle 9: size a deadline
    // from the real cadence, never from a guess). A leg that has not arrived, not
    // been cancelled by combat, and not lost its destination in two minutes is STUCK
    // -- unreachable destination, blocked navmesh, an outranking package we are
    // losing. Two minutes is far longer than any legitimate cross-cell travel we
    // expect to see, so it cannot cut a live leg short. When it fires the leg is
    // dropped and the reason is logged as a FAILURE, loudly and by name -- it is not
    // retried and it is not hidden (principle 7).
    constexpr std::uint64_t kLegMaxMs = 120000;

    // ABI v12: THE BLOCKED END. The engine answers "this actor cannot move along its
    // path" by running a runtime package of type 36, Movement Blocked
    // (PACKAGE_PROCEDURE_TYPE::kMovementBlocked). The type is PACKAGE_DATA::packType,
    // TESPackage+0x24; the engine's own name lookup (TESPackage vfunc 0x39,
    // GetObjectTypeName) is `movsx rax, byte [rcx+0x24]` into a string table whose entry
    // 36 is "Movement Blocked" -- read on BOTH images (1.6.1170 fn 0x496D50 / table
    // 0x200F670, 1.5.97 fn 0x43B790 / table 0x1DEC060). It is that exact label APMF's
    // [obs] line prints, which is how the field freezes were seen.
    //
    // SIZED FROM THE FIELD (deck 2026-09-24, design doc section 4): healthy legs show a
    // Movement Blocked blip of about one [obs] sample (the ~1 s observability cadence)
    // and then arrive; a lever portcullis held two followers in it for 39-45 s. Three
    // seconds of Movement Blocked in RUNNING game time (see kMbMaxStepMs for the clock and
    // its one-missed-poll tolerance) is therefore above every healthy blip seen
    // and ends a real freeze about 40 s sooner than the two-minute stuck net. It is not a
    // deny and not a fight with the engine: the engine's collision answer is observed and
    // the leg is ended as BLOCKED, so the client can decide.
    constexpr std::uint64_t kBlockedEndMs = 3000;

    // THE BLOCKED CLOCK COUNTS RUNNING GAME TIME, NOT THE WALL CLOCK (review F1). Poll runs
    // from the player's update, which does not run while a menu pauses the game, so a wall
    // clock would turn a 1 s blip plus 3 s in the inventory into a false BLOCKED on the
    // first poll after the menu closes. Instead each poll that sees Movement Blocked adds
    // the time since this leg's previous poll, CAPPED at kMbMaxStepMs (two poll periods):
    // a paused stretch contributes at most one capped step, while a real 60 fps game polls
    // every 250 ms and adds that in full. A frame rate so low that polls are > 500 ms
    // apart only makes the end LATER (a floor, never an early end; principle 9).
    // ONE non-Movement-Blocked poll inside a run is TOLERATED (review F6): a flicker of the
    // running package shorter than a poll must not restart a real gate's clock; a second
    // consecutive miss ends the run. A healthy ~1 s blip therefore accrues ~0.75-1 s and is
    // then cleared by two clean polls.
    constexpr std::uint64_t kMbMaxStepMs = 2 * kPollPeriodMs;

    // THE BLOCKER CHECK (review F3, marth 2026-09-24: "do a directional proximity check to
    // see if its another follower, or the player, or an NPC"). At the BLOCKED verdict the
    // nearest live actor IN FRONT of the stalled actor is looked for: centre-to-centre
    // within kBlockerReach on the ground plane, |dz| <= kBlockerMaxDz, and inside a cone of
    // +-kBlockerHalfAngleDeg around EITHER the actor's facing (data.angle.z; Skyrim's
    // forward is (sin z, cos z)) OR the straight bearing to the destination. Why these
    // numbers (an engineering choice, sized to be observed in the field, not an engine
    // constant): a humanoid's collision capsule is a few tens of units across, so two
    // bodies in contact stand well under 100u apart; the engine's avoidance gives up with
    // some room to spare; 192u covers "right in front" plus the body behind it without
    // reaching into the next room. The two cones exist because an actor stuck while turning
    // may face along the path while the blocker stands toward the goal, or the reverse.
    // One floor of height (128u) keeps an actor on a balcony above out of it.
    constexpr float kBlockerReach        = 192.0f;
    constexpr float kBlockerMaxDz        = 128.0f;
    constexpr float kBlockerHalfAngleDeg = 60.0f;

    // ---- Install state (defined in Travel.cpp) ----
    extern RE::TESPackage*          g_pkg[kTravelSlots];
    extern bool                     g_authoredPrefSpeed[kTravelSlots];
    extern std::uint8_t             g_authoredSpeed[kTravelSlots];
    extern bool                     g_gaitVerified;

    // ---- Per-leg state. GAME THREAD ONLY (see the threading note). ----
    // Which kind of destination `destId` names. Decided ONCE, at claim time, from
    // the form's own record type -- never re-inferred later.
    enum class DestKind { kRef, kCell };

    struct Leg {
        RE::ActorHandle       actorHandle{};
        RE::FormID            destId   = 0;
        DestKind              destKind = DestKind::kRef;
        RE::ObjectRefHandle   destHandle{};   // kRef only: a REFERENCE, not necessarily an actor
        float                 radius = kDefaultRadiusUnits;
        std::uint32_t         flags  = 0;
        float                 basis  = 0.0f;
        int                   slot   = -1;                          // -1 == no package slot held
        APMF_API::Handle      hPackage = APMF_API::kInvalidHandle;  // the INTERNAL ch.9 offer
        // The basis the ch.9 offer was actually FILED at. A claim's basis is
        // IMMUTABLE once applied (`ApplyRepoint` updates the param and nothing
        // else), so when the ch.19 winner changes and the new owner's basis differs,
        // the offer does NOT follow by itself -- it has to be re-filed. See Compose.
        float                 basisPackage = 0.0f;
        bool                  legLive = false;
        std::uint64_t         legStartedMs = 0;
        // Was the destination ALIVE when it was targeted (engage or re-point)? Only
        // then does its death end the leg ("the destination died during travel"). A
        // destination already dead at target time is a corpse to walk to (MFO's loot
        // legs), so its deadness never ends the leg. Sampled in Compose, reset on
        // every re-point. False for a cell and until Compose has sampled it.
        bool                  destAliveAtTarget = false;

        // ABI v11 position legs (kTravel_ToPosition). `point` is what the client
        // declared; `marker` is APMF's XMarker for the point `markerPoint` (the two
        // differ only between a Repoint and the Compose that replaces the marker).
        // While a marker exists, destHandle/destId name it. APMF owns the marker and
        // deletes it when the leg ends for any reason.
        bool                  toPosition = false;
        RE::NiPoint3          point{};
        RE::ObjectRefHandle   marker{};
        RE::FormID            markerId = 0;
        RE::NiPoint3          markerPoint{};

        // ABI v12 BLOCKED clock (see kBlockedEndMs / Poll). `mbAccumMs` = Movement
        // Blocked time accumulated over RUNNING polls in the current run (0 = not
        // blocked); `mbLastPollMs` = when Poll last evaluated this leg; `mbMissed` = the
        // previous poll of a run did not see MB (one such poll is tolerated). All reset
        // by ResetBlockedClock at every leg start, re-point and end. `gaitLogged` = the
        // one passive [travel-gait] line for this leg has been written. `ownerHandle` =
        // the winning ch.19 claim's handle when this leg was last composed (reported as
        // APMF_TravelLegInfo::ownerHandle; 0 until the first Compose).
        std::uint32_t         mbAccumMs    = 0;
        std::uint64_t         mbLastPollMs = 0;
        bool                  mbRun        = false;   // a Movement Blocked run is being timed
        bool                  mbMissed     = false;
        bool                  gaitLogged   = false;
        APMF_API::Handle      ownerHandle  = APMF_API::kInvalidHandle;
    };

    void ResetBlockedClock(Leg& a_leg);

    // ---- Per-leg state (defined in Travel.cpp). GAME THREAD ONLY. ----
    extern std::unordered_map<RE::FormID, Leg> g_legs;
    extern RE::FormID                          g_slotOwner[kTravelSlots];
    extern std::atomic<std::uint32_t>          g_legCount;
    extern std::uint64_t                       g_lastPollMs;
    extern RE::FormID                          g_slotMarkerId[kTravelSlots];

    // ---- ABI v12: the LEG-STATE MIRROR (APMF_API_v12::GetTravelLegState) -----------
    // g_legs is game-thread-only and must stay lock-free, so the any-thread read gets its
    // own small record per actor, written by the game-thread paths below at every state
    // change and read by GetLegState under `g_stateMx`. Nothing but these records is under
    // the mutex, and nothing is called while it is held, so it cannot deadlock or stall a
    // frame. Cleared at the load boundary (ResetAll); bounded by the actors that have
    // travelled since the last load.
    struct LegStateRec {
        std::uint32_t state     = APMF_API::kLeg_None;
        RE::FormID    destForm  = 0;
        RE::NiPoint3  point{};
        std::uint64_t sinceMs   = 0;
        std::uint32_t seq       = 0;
        RE::NiPoint3  stall{};
        std::uint32_t blockedMs = 0;
        std::uint32_t speed     = 0xFFFFFFFFu;
        std::uint32_t owner     = 0;
        RE::FormID    blocker   = 0;
        std::uint32_t blockerKind = APMF_API::kBlocker_None;
    };
    extern std::mutex                                  g_stateMx;
    extern std::unordered_map<RE::FormID, LegStateRec> g_state;   // guarded by g_stateMx
    extern std::uint32_t                               g_seqCounter;

    // What a BLOCKED end carries into the mirror (kLeg_Blocked only).
    struct BlockInfo {
        RE::NiPoint3  stall{};
        std::uint32_t ms          = 0;
        RE::FormID    blocker     = 0;
        std::uint32_t blockerKind = APMF_API::kBlocker_None;
    };

    // ---- The leg-state mirror, markers, slots and the leg (TravelLeg.cpp) ----
    std::uint32_t DeclaredSpeed(std::uint32_t a_flags);
    void SetLegState(RE::FormID a_id, std::uint32_t a_state, RE::FormID a_destForm, const RE::NiPoint3& a_point,
                     std::uint32_t a_speed, APMF_API::Handle a_owner, const BlockInfo* a_block = nullptr);
    std::uint32_t MirroredSpeed(RE::FormID a_id);
    void SetLegStateFor(RE::FormID a_id, const Leg& a_leg, std::uint32_t a_state, std::uint32_t a_speed,
                        const BlockInfo* a_block = nullptr);
    bool SamePoint(const RE::NiPoint3& a, const RE::NiPoint3& b);
    void RetireMarkerLater(RE::FormID a_id, RE::ObjectRefHandle a_marker, RE::FormID a_markerId, const char* a_why);
    void DropLegMarker(RE::FormID a_id, Leg& a_leg, const char* a_why);
    void DropOfferAndFreeSlot(APMF_API::Handle a_hPackage, int a_slot);
    APMF_API::Handle RefileOffer(RE::FormID a_id, APMF_API::Handle a_old, float a_oldBasis,
                                 float a_newBasis, const APMF_API::APMF_Param& a_param);
    bool ClassifyDestination(RE::FormID a_id, RE::FormID a_dest, DestKind& out, const char* a_step);
    void EndLeg(RE::FormID a_id, Leg& a_leg, const char* a_why, bool a_failure, std::uint32_t a_state,
                const BlockInfo* a_block = nullptr);
    bool PointPackage(RE::FormID a_id, const Leg& a_leg, RE::TESPackage* a_pkg);
    bool StartLeg(RE::FormID a_id, Leg& a_leg);
    bool StillOurs(RE::FormID a_id, const Leg& a_leg, const char* a_step, float* a_outBasis,
                   APMF_API::Handle* a_outOwner = nullptr);

    // ---- Gait (TravelGait.cpp) ----
    const char* SpeedName(std::uint32_t a_speed);
    std::uint32_t ApplyGait(RE::FormID a_id, const Leg& a_leg, int a_slot, bool a_liveRepoint);
    void ObserveGait(RE::FormID a_id, RE::Actor* a_actor, Leg& a_leg);

    // ---- The running package and the blocker check (TravelBlocker.cpp) ----
    struct RunningPackage {
        RE::FormID formId          = 0;
        bool       movementBlocked = false;
    };
    RunningPackage ReadRunningPackage(RE::Actor* a_actor);
    const char* BlockerKindName(std::uint32_t a_kind);
    void FindBlocker(RE::Actor* a_actor, const Leg& a_leg, BlockInfo& a_blk);

    // ---- Passive gate probe 1 (TravelGateProbe.cpp) ----
    // Probe 1's constants the channel and probe 2 also read; the rest stay in TravelGateProbe.cpp.
    constexpr float         kProbeRadius    = 1024.0f;
    // The watch window (Install's log line).
    constexpr std::uint64_t kWatchMs        = 600000;   // 10 minutes

    struct WatchedRef {
        RE::FormID ref  = 0;
        RE::FormID base = 0;
    };
    struct GateWatch {
        RE::FormID              actor   = 0;
        RE::FormID              space   = 0;   // interior cell, or exterior worldspace
        RE::NiPoint3            stall{};
        std::uint64_t           untilMs = 0;
        std::uint32_t           lines   = 0;
        std::vector<WatchedRef> refs;
    };
    extern std::atomic<bool>          g_probeReady;
    extern std::mutex                 g_watchMx;          // guards g_watches (sinks run on engine threads)
    extern std::vector<GateWatch>     g_watches;
    extern std::atomic<std::uint32_t> g_watchCount;  // the sinks' relaxed pre-gate
    extern std::unordered_map<RE::FormID, std::pair<std::uint64_t, RE::NiPoint3>> g_lastProbe;
    const char* OpenStateName(RE::BGSOpenCloseForm::OPEN_STATE a_s);
    const char* GateTypeName(const RE::TESBoundObject* a_base);
    void OnGateEvent(const char* a_kind, RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_by, int a_opened);
    void GateProbe(RE::FormID a_id, RE::Actor* a_actor, const RE::NiPoint3& a_stall);

    // ---- Passive gate probe 2 (TravelGateProbe2.cpp) ----
    extern std::atomic<bool> g_probe2Ready;
    extern std::uintptr_t    g_vtAnimGraph;
    extern std::uintptr_t    g_vtBehaviorGraph;
    extern std::uintptr_t    g_vtStateMachine;
    extern std::unordered_map<RE::FormID, std::pair<std::uint64_t, RE::NiPoint3>> g_lastProbe2;
    void GateProbe2(RE::FormID a_id, RE::Actor* a_actor, const RE::NiPoint3& a_stall, RE::TESObjectREFR* a_dest,
                    RE::FormID a_destId, bool a_haveDest, const RE::NiPoint3& a_destPos);
