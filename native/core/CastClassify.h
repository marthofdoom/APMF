#pragma once

// ============================================================================
// ch.8b SEAT 0 -- CLASSIFY. The root-cause fix (RE notebook, section J1-J9,
// scratchpad/fable-cast-re-findings.md): a heal/buff-OTHER spell (kTargetActor /
// kAimed RestoreHealth -- Healing Hands, Heal Other, and every delivery-flip
// PROXY the OTHER four engine seats already know how to drive) is NEVER GIVEN a
// CombatInventoryItem by the engine's own classifier. The 23-row table at
// 0x20163b0 keys on (archetype, actorValue, HOSTILE, SELF-DELIVERY); there is NO
// row for (Health, non-hostile, non-self) (J3). The five existing engine seats
// (0x0F/0x06/0x0A/0x07/0x0D -- core/EquipGate.cpp + core/CastSeats.cpp) sit
// downstream of this and were correctly implemented but were STRUCTURALLY IDLE
// for exactly this case (J1/J4/P3.1): they answer questions the engine never
// gets to ask, because no item, no caster, no magic context is ever built for a
// heal-OTHER spell in the first place.
//
// THE FIX. `CombatMagicItemData` -- the per-spell classification RESOLVER the
// engine constructs once per spell evaluation -- sets a SELF-DELIVERY bit at
// `+0x4c` once, at construction, from the spell's OWN `GetDelivery()`. Its
// per-effect visitor (vtable slot 1, 0x81d830 in the 1.6.1170 disassembly)
// reads that bit as PART of the classification lookup key (J2/P3.3). This file
// hooks slot 1: if the effect being classified belongs to the spell a live
// `kIntent_Cast` claim is currently DRIVING for the deliberating actor (the
// SAME "driven form" test core/CastSeats.cpp's `ClaimNamesThisCast` already
// uses -- proxy-when-one-exists, else the claim's spell) AND that EFFECT is
// NOT HOSTILE (2026-09-06: a hostile claim, e.g. Firebolt, already keys into
// an existing table row via its own hostile=1 bit and needs no help -- forcing
// isSelfDelivery=1 on it would misclassify it into a row vanilla never
// populates, since a hostile spell is never self-cast), force `+0x4c = 1`
// BEFORE chaining to the real classifier -- so the claimed spell's effect keys
// into the EXACT SAME table row a self-heal already uses (Restore, creator
// 0x824510). Nothing else about the row, the score, or the duration changes
// (J9/J2: "+0x4c is read ONLY inside this one visitor; forcing it changes
// classification and nothing else").
//
// This is COMPOSITION (Docs/INVARIANTS.md #20), not generation: APMF does not
// create an item, does not choose a caster, does not decide combat eligibility
// beyond "this spell counts as self-delivered for CLASSIFICATION PURPOSES,
// because a live claim already named it as the thing to cast." The engine's own
// table, scoring, equip gate (0x0F) and caster seats (0x06/0x0A/0x07/0x0D) do
// everything else exactly as they do for a vanilla self-heal -- this seat only
// ever WIDENS which row a spell's effects can reach; it never fabricates a row,
// a score, or an eligibility decision the engine did not itself compute.
//
// SCOPE / SAFETY (all required; matches every other composed seat in this
// codebase -- see Docs/INVARIANTS.md #20's "raw offset in a composed seat"
// clause, which this deliberately follows even though there is no CommonLib
// type to static_assert against):
//   * installs on EXACTLY ONE vtable, VTABLE_CombatMagicItemData -- the ONLY
//     class this visitor lives on (J7: "reached only virtually -- no direct
//     call/jmp in .text"; one class, one vtable, unlike the 30-way
//     CombatInventoryItemMagicT<> template family EquipGate/AiCastSeats hook);
//   * install-time verification by MANGLED RTTI NAME
//     (".?AVCombatMagicItemData@@", read off the resolved vtable's own
//     CompleteObjectLocator/TypeDescriptor) -- NOT a DerivesFrom() pointer-
//     identity walk, because no RTTI_CombatMagicItemData Address-Library ID
//     was established during the RE pass (a documented, explicit gap -- see
//     the .cpp). A name mismatch REFUSES installation outright, never a blind
//     vtable write;
//   * a per-call vtable-identity re-check -- the SAME "foreign vtable ->
//     recover the live original, never fabricate" pattern every seat in this
//     codebase already uses (a lookup miss can only happen if this file never
//     wrote that slot, i.e. structurally, since the slot is only ever entered
//     through the address this file itself installed);
//   * an INI kill-switch, `[CastSeats] EnableSeat0Classify` (default 1); and
//   * AE-ONLY, explicitly refused on SE and VR: the three struct offsets this
//     thunk reads (+0x10 spell, +0x18 CombatController*, +0x4c self flag) are
//     disassembly-CERTAIN on 1.6.1170 (AE) and explicitly NOT VERIFIED on
//     1.5.97 (SE) per the RE notebook (J9). Per CLAUDE.md's rule, an unverified
//     struct layout is refused explicitly at install, never silently assumed
//     to carry across runtimes.
//
// See core/CastClassify.cpp for the exact offsets, the mangled-name evidence,
// and the log-line shape (rate-limited, INI-gated, matching every other probe
// in this codebase).
// ============================================================================

namespace apmf::castclassify {

    // Install SEAT 0 on the CombatMagicItemData vtable. Call at kDataLoaded.
    // Ordering relative to core/EquipGate.cpp / core/CastSeats.cpp does not
    // matter for correctness (different vtable entirely) -- SEAT 0 only ever
    // widens which table row a spell's effects reach; the other five seats
    // answer for whatever object the table then mints. VR- and non-AE-refused
    // (see the file banner). Idempotent / install-once.
    void Install();

}
