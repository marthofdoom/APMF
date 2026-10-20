#pragma once

// ============================================================================
// THE TORCH SEAT (2026-10-20, feat/apmf-torch-seat, ClickUp 86e3qmmed item H1).
//
// The engine has ONE torch decision outside combat: the torch updater (AE id 39948 /
// SE 38902 / 1.7.104 0x7265C0), `void(AIProcess* process, Actor* actor)`, called from
// exactly one place, the per-actor AI update (AE 39637+0x8B1 / SE 38606+0x798 /
// 1.7.104 0x704C70+0x8B1; non-player actors only). It has two branches:
//   A (every update): while the actor is seated, sleeping, has ExtraInteraction or a
//     furnitureIdle, it UNEQUIPS a held torch (UnequipObject 38901 at +0x236).
//   B (out of combat, fTorchEvaluationTimer, 5 s): ShouldUseTorch (37567) decides and it
//     EQUIPS (+0x199, the sink sees it as path=TorchUpdate) or UNEQUIPS the torch.
// The sink (core/EquipSink.cpp) sees B's equip but NOT the unequips (kEquipAuth_DenyUnequip
// is RESERVED), so on 2026-10-18 a seated follower's declared torch was stowed every ~1 s
// with no Harbinger line at all (agentlogs/diag-torch.md).
//
// marth 2026-10-20: "1 is fine, but it still violates harbinger blocking every other
// source when its claim is open." So WHILE the actor's winning ch.17 claim OWNS
// kEquipCat_Light (owned, not denied, with a declared set; not observe-only) and outranks
// the Engine source on the EquipWeapon facet, the call to the torch updater is SKIPPED:
// both branches are denied. Harbinger equips nothing here (#17a condition 4): the client
// declared the torch and the existing ch.17 enforce pass equips it. When no claim owns
// Light the updater runs untouched; for an actor with a ch.17 claim that does not own
// Light, an engine stow is LOGGED (passive, rate-capped), so it is no longer invisible.
// Gating is the CLIENT's: MFO does not declare a torch while its follower sits, sleeps or
// uses furniture, so the engine's stow then happens with no claim owning Light.
//
// THE SEAT: write_call<5> on that one E8 (INVARIANTS #17a, amended 2026-10-20 with this
// seat's own argument). Exact 1.6.1170 / 1.5.97 / 1.7.104, VR-refused, INI
// [EquipAuthority] bTorchDeny (default 1; 0 = no seat, the engine's torch behaviour is
// untouched everywhere), SeatVerified on the site row and the target row, and the E8 is
// re-read at install (it must still call the verified updater) before anything is written.
// THREAD: whatever thread runs the engine's AI update for that actor (logged once on the
// first call). The thunk reads only the lock-free ControlMap snapshot (TryGetEquipSet), the
// source registry's priorities (its shared lock) and its own leaf mutex for the log state.
// ============================================================================

namespace apmf::torchseat {

    // kDataLoaded, after the self-check. Install-once.
    void Install();

    // The seat is patched (for the log / menu). Any thread.
    bool Installed();

}  // namespace apmf::torchseat
