#pragma once
#include "APMF_API.h"

// ============================================================================
// APMF core -- POSITION CAST (ABI v11, kCastFlag_AtPosition).
//
// A client's kIntent_Cast RequestEx can name a WORLD POINT instead of an actor.
// APMF places a non-persistent XMarker (Skyrim.esm 0x3B) at the point and casts
// the spell FROM that marker, blamed on the client's actor, with the exact call
// sequence the engine's own Papyrus Spell.RemoteCast uses:
//     marker->GetMagicCaster(kInstant)->InterruptCast(false);
//     caster->CastSpellImmediate(spell, false, nullptr, 1.0f, false, 0.0f, actor);
// then deletes the marker one main-thread hop later (Disable + SetDelete(true)).
//
// WHY THE MARKER IS THE CASTER (disassembly, both runtimes; the full table is
// Docs/ADDRESS-TABLE-2026-09-15.md, ADDENDUM 2026-09-23). The engine's cast core
// (AE 0x5bc160 / SE 0x54cd10) places a Target Location spell at the CASTER's own
// magic node, or its position when it has none. It never reads the target it was
// handed for that delivery. An actor "casting at a marker" therefore lands at the
// actor's hand. A marker's NonActorMagicCaster has no magic node and reports no
// out-actor, so the location is the marker itself and no navmesh snap moves it.
//
// WHAT IS REFUSED, BY NAME: summons (the engine applies a summon effect only to the
// actor that cast it, so a marker can never summon), every delivery other than
// Target Location, concentration and constant-effect spells, the three spell types
// Papyrus RemoteCast refuses (disease, ability, addiction), any other cast flag set
// alongside kCastFlag_AtPosition, an unloaded or dead actor, a cell that is not
// attached. Every refusal is one WARN line naming the reason. Nothing is retried.
//
// DOCTRINE (Docs/INVARIANTS.md #0 action (e), added with this module). This is the
// one place APMF itself makes a cast call. It is legal because no decision seat
// exists to compose instead (an NPC's AI has no seat that aims a location spell at
// a point, and seat 0x0A carries an Actor*), because the client declared both the
// spell and the point, and because it is a single one-shot call on the main thread
// with no re-assert. It is NOT a claim: it never enters the control map, so no seat
// can ever read it as an actor target, and it holds no facet.
//
// MARKER LIFETIME. Each marker lives from its cast to the next main-thread pump
// (one frame). It is tracked by its ObjectRefHandle (the handle carries age bits,
// so a recycled 0xFF FormID can never be mistaken for it) and re-checked before
// deletion: same FormID, base still the XMarker. At most kMaxLiveMarkers exist at
// once; a request past that is refused. Revert and kPreLoadGame forget the table
// without touching any reference (the old world is going away). The worst-case
// save footprint: a save taken inside that one frame captures at most one marker
// per position cast issued in it (never more than kMaxLiveMarkers), each an inert
// XMarker reference that nothing will delete later.
//
// THREADING. Enqueue() is safe from any thread (it copies POD and posts). Every
// other function is MAIN-THREAD ONLY (apmf::mainthread::Pump, or the SKSE
// revert / kPreLoadGame handlers, which run on the main thread).
// ============================================================================

namespace apmf::poscast {

    // At most this many markers exist at once (see MARKER LIFETIME above).
    inline constexpr std::size_t kMaxLiveMarkers = 16;

    // kDataLoaded: runtime gate (exactly 1.6.1170 or 1.5.97, never VR), the INI
    // switch ([PositionCast] bPositionCast, default 1), and the XMarker base form.
    // Logs the outcome once. Idempotent.
    void Install();

    // True once Install() succeeded. Any thread.
    bool Installed();

    // Why Install() refused ("" when installed). Any thread.
    const char* NotInstalledReason();

    // ANY THREAD. Validate what can be validated from POD alone and post the
    // delivery to the main thread. Returns false (and logs the reason) when the
    // request is refused synchronously; the caller then returns kInvalidHandle.
    // `handle` is only a label for the log lines.
    bool Enqueue(APMF_API::Handle handle, RE::FormID actor, const APMF_API::APMF_Param& param);

    // MAIN THREAD. Forget every tracked marker WITHOUT touching it (revert /
    // kPreLoadGame: the references belong to the world being replaced). Logs how
    // many were abandoned.
    void ResetAll(const char* why);

}
