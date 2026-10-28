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
// TWO MORE ANSWERS (feat/apmf-buff-summon-seats, 2026-10-05):
//   * PLACEMENT SPELLS (Summon / Reanimate) are never forced -- their native key
//     is the right one, and Reanimate's only row is the non-self one.
//   * ROWLESS ROW SUBSTITUTION: a claimed spell none of whose effects keys a
//     caster row (Muffle, fortify / resist, Night Eye, Detect Life, cures, Calm /
//     Frenzy / Fear / Courage, ...) is handed to the engine's own KEEP-BEST with
//     the engine's own Script row of the same hostility, so the AI builds the
//     generic Script caster for it and the claim seats answer it. The hand still
//     casts the spell as authored. See ServeRowless in the .cpp.
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

    // A spell with a SummonCreature or Reanimate effect (feat/apmf-buff-summon-seats, ClickUp
    // 86e3dvkwm). Seat 0 never forces its self flag (Reanimate's only row is the non-self one),
    // and ControlMap::ApplyRequest resolves a target-0 claim on it to the claimant (the caster
    // type places it: the summon's own placement, Reanimate's own corpse). Any thread; reads
    // form data only.
    bool IsPlacementSpell(const RE::MagicItem* a_spell);

    // ---- EXPLAINED-ZERO (ClickUp 1dfcuabbt0n / 1dfcuabbt0p, marth 2026-10-27 Q1) ----------------
    // The engine's magic item score (shared slot 0x0C: AE 0x819AA0 -> 0x819C10, SE 0x77F6F0 ->
    // 0x77F890, 1.7.104 0x82E980 -> 0x82EAF0) is -1.0 when no effect keys a row, else the SUM of the
    // per-effect row terms times the combat style's magicScoreMult and a range bonus. Each row's
    // term is one of three DATA reads (row fn+0x20 / fn+0x28, identical on all three runtimes,
    // checked against the live table and the code bytes at Install):
    //   magnitude -- the Effect's magnitude, 0 when the MGEF has kNoMagnitude (most rows)
    //   cost      -- Effect::cost (+0x18): Paralyze, Summon (both), Invisibility, BoundItem
    //   aiScore   -- EffectSetting data.aiScore (+0x14C): the two Script rows (fn+0x20 is discarded)
    // An exact 0.0 is EXPLAINED when every row an effect actually keyed during THAT scoring call has
    // a zero data term. Seat 0's visitor thunk records what it saw (each effect, the self flag the
    // engine keyed with, a row this file served by substitution) while the 0x0C thunk has the capture
    // armed. Thread-local, so no lock. Nothing here changes a classification.
    struct ScoreCapture {
        static constexpr std::size_t kMax = 8;
        struct Entry {
            const RE::Effect* effect = nullptr;
            std::uint8_t      self   = 0;    // the resolver's +0x4c as the engine keyed this effect
            std::int8_t       served = -1;   // table row this file substituted for it (heal / rowless), else -1
        };
        const RE::TESForm* spell    = nullptr;
        std::uint8_t       count    = 0;
        bool               overflow = false;
        Entry              e[kMax]{};
    };
    // Arm before the engine's 0x0C original, for the item form being scored. End disarms and copies
    // what seat 0 recorded. Any thread (thread-local), cheap enough for every magic score call.
    void BeginScoreCapture(const RE::TESForm* a_item);
    void EndScoreCapture(ScoreCapture& a_out);

    struct ZeroExplain {
        bool         explained = false;
        std::int32_t row       = -1;   // the first row index (0-22) an effect keyed or was served
        const char*  term      = "";   // "magnitude" / "cost" / "aiScore" for that row
        RE::FormID   mgef      = 0;    // that effect's MGEF
        std::uint8_t effects   = 0;    // effects seat 0 saw
        std::uint8_t terms     = 0;    // row terms evaluated
        const char*  why       = "";   // the refusal reason when !explained (a static string)
    };
    // Is an exact-0 engine score for `a_spell` explained by its own data? False (with `why`) when the
    // model is not verified on this runtime, seat 0 saw nothing for this form, an effect keyed a row
    // the model does not cover, or any keyed row's data term is non-zero (then the zero came from
    // outside the form: combat style, range, resistance). Form reads only, any thread.
    bool ExplainZero(const ScoreCapture& a_cap, const RE::TESForm* a_spell, ZeroExplain& a_out);
    // True once Install verified the term model on this runtime (logged either way).
    bool ZeroModelVerified();

}
