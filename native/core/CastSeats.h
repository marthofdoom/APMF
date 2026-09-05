#pragma once

// ============================================================================
// ch.8b -- THE ENGINE CAST SEATS. The keystone of the cast facet: while a
// `kIntent_Cast` claim {actor A, spell S, target T} stands, APMF answers the
// vfunc seats the combat AI's OWN cast decision is BUILT OUT OF, so that the
// NPC's own AI selects, equips, charges, aims, fires and channels S at T --
// with the engine's own animation, timing, magicka, LOS and interrupt handling.
// APMF makes NO cast write: no EquipSpell, no CastSpell/CastSpellImmediate, no
// NotifyAnimationGraph, no MagicCaster state poke. Every one of those is the
// engine's, from the engine's own behavior tree. This module only ANSWERS.
//
// It REPLACES the forced-cast drive that used to live in core/CastExecutor.cpp
// (PhaseSelect -> PhaseRest -> PhaseDrawn -> PhaseFire -> PhaseHold -> ParkHand
// + a CastSpellImmediate fallback), which is retired in the same pass.
//
// ── THE FIVE SEATS (RE: the 1.6.1170 disassembly notebook "cast-on-friendly")──
// FOUR live here (the CombatMagicCasterRestore vtable); the fifth, the SELECTION
// gate, lives in core/EquipGate.cpp because that file already owns slot 0x0F.
//
//   0x0F CheckShouldEquip   (core/EquipGate.cpp)  -- WHICH item enters the hands
//   0x06 CheckStartCast     (here)                -- WHETHER to start the cast
//   0x0A GetMagicTarget     (here)                -- WHERE it is aimed/applied
//   0x07 CheckStopCast      (here)                -- HOW LONG a channel runs
//   0x0D SetupAimController (here)                -- the projectile/facing aim
//
// ── WHY THE 0x0F SEAT IS THE ONE THAT UNBLOCKS EVERYTHING ───────────────────
// The five concrete Restore ITEM templates OVERRIDE CheckShouldEquip with
// `CombatInventoryItemMagic::CheckShouldEquip (return true) && 0x81f7c0(ctrl,
// item)`. That static pre-check has NO caster object -- it reads its target
// DIRECTLY off the CombatController (`delivery == kSelf ? ctrl.attacker :
// ctrl.TARGET`) and runs `ShouldRestore` on it. So a HEALTHY follower fighting a
// HEALTHY foe never lets a heal into its equipment set at all, the magic context
// is never built, and 0x06/0x0A/0x07 are NEVER CALLED for that spell. Redirecting
// GetMagicTarget alone is necessary and NOT sufficient -- 0x0F is the seat one
// level upstream, and it is the only one of the five that cannot be answered by
// redirecting an input, because there is no interposable seat between it and the
// CombatController fields it reads. It is therefore answered FROM THE CLAIM,
// WITHOUT chaining. That is a deliberate, narrowly-scoped exception to this
// codebase's otherwise universal "chain unconditionally, only ever flip YES->NO"
// rule -- see Docs/INVARIANTS.md #20, which states the exception and its three
// conditions. Every OTHER seat in APMF still chains.
//
// ── SCOPE IS THE SAFETY ARGUMENT ────────────────────────────────────────────
// `GetMagicTarget`'s implementation (0x81e020) is the BASE, SHARED by 13 of the
// 14 caster vtables -- including Stagger, Disarm and Offensive. An unscoped
// redirect would aim HOSTILE effects at the ally. So:
//   * these seats install on `VTABLE_CombatMagicCasterRestore` ONLY (never the
//     shared base, never the other 13), RTTI-verified at install; and
//   * every thunk additionally requires an exact match on the DRIVEN FORM
//     (`this->magicItem == the claim's proxy-or-spell`) for the deliberating
//     actor. Two independent gates, either of which alone would suffice.
// A Restore caster that is not the claim's is untouched; it chains.
//
// ── kSelf SPELLS STILL NEED THE PROXY ───────────────────────────────────────
// `FindTargets`' Self branch (0x5bc98a) resolves to the caster's OWN reference
// and never reads `desiredTarget`, so no seat can aim a kSelf heal at an ally.
// `ControlMap::ApplyRequest` mints a kTargetActor delivery-flip copy for that
// case (core/CastProxy.h) and the seats drive THAT form. kTargetActor spells
// (Healing Hands) need no proxy: seat 0x0A's handle becomes `desiredTarget`.
//
// ── THREADING ───────────────────────────────────────────────────────────────
// All four thunks run on the COMBAT thread. Each does exactly one lock-free RCU
// read (`ControlMap::TryGetCastSeatClaim`), touches no follower list, takes no
// mutex, and reads only `CombatController` members BELOW the AE +0x68 layout
// divergence (`attackerHandle` 0x28) -- static_assert'd in the .cpp. The target
// is carried as a pre-resolved native `ActorHandle` so no thunk ever runs a form
// lookup (which would take the engine's forms-map lock).
//
// ── CONFIG ──────────────────────────────────────────────────────────────────
// `Data/SKSE/Plugins/APMF.ini`, section `[CastSeats]`:
//   EnableAimSeat=0|1   (default 1) -- kill-switch for seat 0x0D ONLY. The
//     `CombatProjectileAimController::+0x30` target override is the one field
//     here reached by a raw offset rather than a pinned CommonLib member (the
//     class has no header in the pinned rev -- only a forward declaration), so it
//     gets its own switch AND a runtime vtable-identity check before any write.
//     See the .cpp's guard. The other four seats have no flag: they are pure
//     vfunc answers on pinned, RTTI-verified symbols.
// ============================================================================

namespace apmf::castseats {

    // Install the four caster seats (0x06/0x07/0x0A/0x0D) on the Restore caster
    // vtable. Call at kDataLoaded, AFTER core/AiCastSeats.cpp's passive probe so
    // that probe keeps observing the ENGINE's raw answer beneath these (write_vfunc
    // chains newest-first: last installed sits outermost). VR-refused (the vtable
    // indices are SE/AE-verified only). Idempotent / install-once.
    void Install();

}
