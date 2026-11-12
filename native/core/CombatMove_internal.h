#pragma once
#include "APMF_API.h"
#include "core/Sources.h"

#include <string>

// ============================================================================
// ch.31 COMBAT MOVE -- the state core/CombatMove.cpp shares with its sibling files (core/CombatMoveBlock.cpp,
// the ABI v43 block hold). INTERNAL: only files of this subsystem include it. The public header is
// core/CombatMove.h.
// ============================================================================
namespace apmf::combatmove::detail {

    struct Sent {   // what the game-thread step measured, written into the record with its verdict
        std::uint32_t info         = 0;
        float         distance     = -1.0f;
        float         bearingDeg   = -1.0f;
        float         staminaSpent = 0.0f;
    };

    // Game thread: finish a record (state, why, blocker, the measurements; `a_sent.info` is ORed in). Returns
    // false when the record is gone (a reset since the post): the caller then logs nothing more.
    bool Finish(APMF_API::Handle a_handle, std::uint32_t a_state, std::uint32_t a_why, APMF_API::Handle a_blocker,
                const Sent& a_sent);

    // Any thread (the leaf lock): set / clear info bits of a record, optionally its hitTarget. False = no record.
    bool MarkInfo(APMF_API::Handle a_handle, std::uint32_t a_set, std::uint32_t a_clear, RE::FormID a_hitTarget = 0);

    // Game thread: a held block is over. Started -> kCombatMoveState_Ended with `a_why`, kCombatMoveInfo_HoldLive
    // cleared. False = no record, or the record is not a live hold.
    bool EndHold(APMF_API::Handle a_handle, std::uint32_t a_why);

    // The world generation (ResetAll bumps it; a posted step of an older one drops itself).
    std::uint32_t Gen();

    const char* MoveName(std::uint32_t a_move);
    const char* WhyText(std::uint32_t a_why);

    // GAME THREAD. The engine's action road (core/CombatMove.h "THE ENGINE ROAD"): a TESActionData on this
    // stack {source = the actor, priority 2, action form = `a_action`}, ONE Process() call, the destructor.
    // `a_event` non-null = the animation event is named (StartAttack's shape: an attack entry's event);
    // null = none is named and the engine picks the action's own idle (the shape of the combat AI's block,
    // AE 44446 0x7F9770). `a_ranEvent` non-null receives the event the data held after Process (the idle the
    // engine picked, or the named one).
    bool ProcessAction(RE::Actor* a_actor, RE::BGSAction* a_action, const RE::BSFixedString* a_event,
                       std::string* a_ranEvent = nullptr);

    // The action forms resolved at Install (BGSDefaultObjectManager, RTTI-checked). Null before Install or when
    // the default object did not resolve.
    RE::BGSAction* ActionLeftAttack();
    RE::BGSAction* ActionLeftRelease();

    // ---- core/CombatMoveBlock.cpp (ABI v43) ----
    // GAME THREAD (the posted step). kCombatMove_Block: every check, the Left Attack action, the read-back, the
    // hold record. kCombatMove_BlockEnd: lower the calling client's hold.
    void RunBlock(APMF_API::Handle a_handle, RE::FormID a_actor, float a_holdS, apmf::sources::SourceId a_client);
    void RunBlockEnd(APMF_API::Handle a_handle, RE::FormID a_actor, apmf::sources::SourceId a_client);
    // Revert / kPreLoadGame, from ResetAll: forget every hold (their records are forgotten with the table).
    void ResetHolds();
    // The hit's thread (PODs only): a hit ON `a_target` carried the Blocked flag. Marks the live hold's record.
    void NoteBlockedHit(RE::FormID a_target, RE::FormID a_cause);

}
