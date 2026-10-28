#pragma once
#include "APMF_API.h"
#include "core/CastClassify.h"   // ScoreCapture (the explained-zero lift)

#include <cstdint>

// ============================================================================
// APMF core -- ch.26 EQUIP PREFERENCE (kIntent_EquipPreference, ABI v22, 2026-10-12): the
// weighting rework, phase 1 (MFO Docs/WEIGHTING-INVENTORY.md "PHASE 1 PLAN", marth's decisions
// D1-D5 of 2026-10-12).
//
// THE MECHANISM. No new seat and no new address: the bias rides the CalculateScore (vfunc 0x0C)
// seats core/AiCastSeats.cpp already installs and verifies -- GROUP C (Melee / Ranged / Shield /
// Torch) and GROUP A (the 30 spell/staff item vtables). Each thunk calls the engine first, then
// hands the engine's number to OnScore below, which returns it unchanged unless the scored item
// is LIFTED:
//   * an item a winning ch.26 claim on the actor DECLARED for that hand (SetEquipPreference), or
//   * (GROUP A only) the form a live ch.8b kIntent_Cast claim on that hand DRIVES -- the spell
//     score steer, which moved here from AiCastSeats' additive +1000 (W2 of the plan).
// A lifted item returns max(engine, best * (1 + kEquipPrefStepRel) + kEquipPrefStepAbs), where
// `best` is the highest score the engine HOLDS for another item in the same engine category
// list (CombatInventory::inventoryItems[GetCategory()], read in place: the caller is that very
// list's rescore loop). Items the actor's preferences / cast claims lift at an equal or better
// rank are left out of `best`, so two lifted items never chase each other upward (D1).
//
// NEVER LIFTED (each a counted state, APMF_API::EquipPrefState): an engine score <= 0 (D1); a
// declared weapon for a hand a live cast claim holds (handblock::HeldFor, D3); any item while a
// source inside the score seat outranks the claim (facetattr::Outranks on the Equip facet, or
// the Cast facet for the spell steer -- D5, "bound by the same priority sliders"); an engine
// list this code cannot validate (Unreadable, logged).
//
// ESCALATION (kEquipPref_Force, D4). The equip sink (core/EquipSink.cpp) asks ForcedFor /
// ForceRefusal for every governed engine equip of a forced actor and refuses a non-preferred
// item into a preferred hand, ranked by priority. A refusal the sliders forbid is a FORCE
// ANOMALY: not applied, logged at warn, counted (menu + heartbeat + APMF_EquipPrefInfo).
//
// ENGINE LAYOUT READ HERE (disassembled 2026-10-12 on all three executables, this branch's agent
// log; also CommonLib's own declarations, static_asserted in the .cpp):
//   CombatController+0x10 = CombatInventory*  (AE 0x558A80 `mov rcx,[rdi+0x10]; call 0x80F380`,
//                                              SE 0x4FD3EB -> 0x7754D0, 1.7.104 0x5608F4 -> 0x824260)
//   CombatInventory+0x00  = parentController; +0x08 + cat*0x18 = inventoryItems[cat] (BSTArray:
//                           data +0, size +0x10); 7 categories (AE 0x80F3E2..0x80F422, SE
//                           0x777A6E..0x777AAF, 1.7.104 0x8242C2..)
//   CombatInventoryItem+0x18 = itemScore (the store right after each 0x0C call: AE 0x80F415,
//                           SE 0x777AA7); +0x10 item form; +0x20 itemSlot (equipSlot at +0x28)
//   vfunc 0x0B GetCategory = int(this), a leaf on every concrete item class on all three builds
//                           (call site AE 0x811BA1 `call [rax+0x58]; movsxd rcx,eax`).
//
// THREADING. OnScore: the combat thread that runs the actor's CombatInventory rescore (the
// caller). No lock is held across an engine call: the reads are the ControlMap RCU snapshot, this
// file's own published declaration snapshot and the engine list in place; facetattr::InsideSource
// and sources::ClaimPriority take leaf SHARED locks; the per-actor state table and the log dedup
// take one small LEAF mutex AFTER every engine read. NoteClaim/Set/Forget/ResetAll: any thread
// (one writer mutex; Set only queues). ForcedFor/ForceRefusal: any thread (the equip worker's
// thread), snapshot reads only. GetState: any thread (the state mutex). Pump (the form lookup of
// the type check, the IsClaimLive prune): MAIN thread only.
// ============================================================================

namespace RE {
    class Actor;
    class BGSEquipSlot;
    class CombatController;
    class CombatInventoryItem;
}

namespace apmf::equippref {

    // ---- arming (core/AiCastSeats.cpp Install, kDataLoaded, main thread) ------------------
    // `steer` = [AiCastSeats] EnableScoreSteer (default 1 since ABI v22, D5); `weaponSeats` /
    // `magicSeats` = how many GROUP C / GROUP A CalculateScore vtables are hooked; the two hand
    // slots resolved there (for the hand of an item in the engine list).
    void Arm(bool steer, int weaponSeats, int magicSeats, RE::BGSEquipSlot* leftHand, RE::BGSEquipSlot* rightHand);
    bool SteerOn();   // the steer is armed (flag on and at least one score seat hooked)

    // ---- the client API (core/ClientAPI.cpp, core/ControlMap.cpp) -------------------------
    // Why a kIntent_EquipPreference request must be refused (nullptr = accept). Any thread.
    const char* RefuseReason(RE::FormID actor, const APMF_API::APMF_Param* param);
    // ClientAPI: a kIntent_EquipPreference Request/RequestEx returned `h`. Only such a handle may declare.
    void        NoteClaim(APMF_API::Handle h);
    // Any thread. Stores the declaration as PENDING (a silent no-op for a handle NoteClaim never saw or
    // that already ended); the main thread type-checks and publishes it next frame (Pump).
    void        Set(APMF_API::Handle h, const APMF_API::APMF_EquipPrefEntry* entries, std::uint32_t count);
    void        Forget(APMF_API::Handle h);   // a client Release (a no-op for a non-ch.26 handle)
    void        ResetAll(const char* why);    // revert / new game
    std::uint32_t GetState(RE::FormID actor, APMF_API::APMF_EquipPrefInfo* out);

    // ---- the score seats (core/AiCastSeats.cpp) -------------------------------------------
    struct ScoreOutcome {
        bool        lifted    = false;   // the returned score is above the engine's
        bool        castSteer = false;   // the lift came from a ch.8b cast claim (not a ch.26 preference)
        char        hand      = '?';     // 'R' / 'L' / '?'
        float       rival     = 0.0f;    // the best competitor's held score (0 = none)
        int         category  = -1;
        bool        zeroLift  = false;   // an exact-0 engine score lifted because its own data explains it
    };
    // Returns the score to hand back to the engine. `a_engine` is the engine's own answer (the
    // thunk called the original first); `a_origFn` the function the thunk chained to (its
    // source is what the claim is ranked against); `a_magicSeat` true for GROUP A.
    // `a_zeroCap` (GROUP A only, else null): what seat 0 saw while the original ran
    // (core/CastClassify.h). With it, an exact-0 score of a ch.8b claim's driven form whose own data
    // explains the zero is lifted like any other claim (marth 2026-10-27, ClickUp 1dfcuabbt0n); every
    // other zero is refused (D1).
    float OnScore(RE::CombatInventoryItem* a_this, RE::CombatController* a_cc, RE::Actor* a_actor, float a_engine,
                  std::uintptr_t a_origFn, bool a_magicSeat, ScoreOutcome& a_out,
                  const apmf::castclassify::ScoreCapture* a_zeroCap = nullptr);

    // ---- the equip sink (core/EquipSink.cpp), kEquipPref_Force ----------------------------
    struct ForceView {
        APMF_API::Handle    handle = APMF_API::kInvalidHandle;
        std::uint32_t       count  = 0;
        APMF_API::APMF_EquipPrefEntry entries[APMF_API::kMaxEquipPreference]{};
    };
    // Cheap pre-gate: is any ch.26 declaration published at all? Any thread, one relaxed load.
    bool AnyDeclared();
    // The winning ch.26 claim on `a_actor` when it carries kEquipPref_Force and a declaration.
    bool ForcedFor(RE::FormID a_actor, ForceView& a_out);
    // The hand bits (kEquipCat_Right / kEquipCat_Left) of `a_competes` for which the forced
    // declaration prefers something and `a_item` is NOT one of that hand's preferred items; 0 when
    // `a_item` is preferred for any hand it competes for (a one-hander with an EitherHand slot).
    std::uint32_t ForceRefusal(const ForceView& a_view, RE::FormID a_item, std::uint32_t a_competes);
    void NoteForceDeny(RE::FormID a_actor);
    void NoteForceAnomaly(RE::FormID a_actor);
    // F1 (review of 7cda290): the forced preference's OWN client equipped the item (it ties with its own
    // claim and stands). Not an anomaly: counted separately, logged at info by the sink.
    void NoteForceOwnClient(RE::FormID a_actor);

    // ---- observability ---------------------------------------------------------------------
    struct Totals {
        std::uint64_t lifts = 0, castSteers = 0, leading = 0, zero = 0, held = 0, withheld = 0, unreadable = 0,
                      forcedDenies = 0, forceAnomalies = 0, forceOwnClient = 0, lostPasses = 0, ignoredForms = 0,
                      droppedDecls = 0, zeroLifts = 0, zeroRefused = 0;   // the explained-zero lift (ch.8b only)
    };
    Totals GetTotals();   // any thread (relaxed counters)
    // MAIN thread, once per frame: type-checks and publishes pending declarations, prunes the
    // declarations of ended claims (1 s), and writes the 30 s heartbeat line.
    void   Pump();
}
