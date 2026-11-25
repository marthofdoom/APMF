#pragma once
#include "APMF_API.h"

namespace apmf::moveorder {

    // NO NEW MELEE ATTACK WHILE A STRICT ORDER TRAVELS OR RETURNS (core/MoveSuppress.cpp). A running swing is never cut.

    // The actor's own combat thread (core/MoveOrder.cpp's pick). `a_actor` may be null only with a_travel false.
    void NotePick(RE::FormID a_fid, RE::Actor* a_actor, bool a_travel, bool a_stood, std::uint64_t a_worldMs);

    // Combat thread (core/ActionGate.cpp's attack leaves). One relaxed load while no strict order is live.
    bool SuppressAttack(RE::FormID a_fid, std::uint8_t a_kind);

    // Any thread (ch.31's start on the game thread): a started bash or power attack is refused now. `a_order` = the
    // strict claim.
    bool SuppressCombatMove(RE::FormID a_fid, APMF_API::Handle& a_order);

    // Any thread: a live strict order's claim handle for `a_fid` (core/MoveOrder.cpp), or kInvalidHandle.
    bool             AnyStrict();
    APMF_API::Handle OrderOwner(RE::FormID a_fid);

    void SuppressReset();       // GAME THREAD: the world is being replaced
    void SuppressHeartbeat();   // GAME THREAD

}
