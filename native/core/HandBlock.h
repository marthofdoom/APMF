#pragma once

#include <cstdint>
#include <string>

// ============================================================================
// HAND-CLAIM BLOCK (fix/apmf-hand-claim-blocks-equip, 2026-10-06).
//
// marth, verbatim: "harbinger taking a hand for an action means other actions on
// that hand are blocked for the duration, thats been standard harbinger
// procedure. And it will stay until we redo using the weighting, which will
// accomplish the same thing."
//
// A hand held by a LIVE kIntent_Cast claim (a driving claim or a
// kCastFlag_DenyHandOnly floor, read per hand by ControlMap::TryGetCastClaimForHand,
// lapsed claims excluded) refuses every OTHER equip into that hand for the claim's
// duration: a weapon, a shield or a torch. The spell/staff half of the same rule
// already stands at core/EquipGate.cpp (0x0F on the 30 magic/staff item vtables);
// this file closes the weapon half on the combat AI's decision seat, and
// core/EquipSink.cpp closes it on the engine-equip worker (every other path,
// and the backstop for a combat equip chosen before the claim was published).
//
// FIELD ORIGIN (Deck 2026-10-06, _research/field-1006-heal-diagnosis.md cause 2):
// MFO released its right hand to a deny-only floor while a heal held the left; 32 ms
// later the engine equipped a Royal Elven Dagger into that right hand
// (`[equip-obs] path=CombatNode verdict=allow`) and the left heal was interrupted.
// Both seats said yes: the weapon leaves' 0x0F was unhooked (the engine's own
// `!IsFleeing` answer), and the sink only governed the ch.17 claim's own scope
// (owned=Armor).
//
// THE SEAT (this file): CombatInventoryItem::CheckShouldEquip, vtable slot 0x0F, on
// the four weapon-class leaves Melee / Ranged / Shield / Torch, plus the unarmed
// OneHandedBlock item (review F1). CombatInventory's
// evaluate (AE 44899 / SE 43666 / 1.7.104 same body) and pre-loop (AE 44868 / SE
// 43637) call it for every candidate and SKIP the item on NO (`call [rax+0x78]; test
// al,al; je`), so a NO keeps the item out of the equipment set and the behaviour
// tree's EquipObject leaf (AE 48124) never receives it. Engine answer first: the
// original is always called and only its YES is ever turned to NO (INVARIANTS #17).
//
// SPELLS-ONLY FLOORS (ABI v20, fix/apmf-floor-spells-only, 2026-10-06). A
// kCastFlag_DenyHandOnly floor that ALSO carries kCastFlag_FloorSpellsOnly reserves its
// hand against spells only (APMF_API.h, that flag): for this rule such a floor holds the
// hand ONLY against a spell-like item (SpellLike below: a spell, a scroll, a staff), so a
// one-handed weapon, a shield or a torch in the floored hand passes both halves. A
// two-hander, a bow or an item the engine has not given a hand (unarmed) still competes for
// the other hand too and is refused while a driving claim holds it. Field 2026-10-06:
// Cicero's floored right hand refused his bow, swords and fists 119 times in 40 s. A
// driving cast claim and a floor without the bit keep the full block.
// ============================================================================

namespace apmf::handblock {

    // kDataLoaded, AFTER equipgate::Install and BEFORE aicastseats::Install (so each
    // weapon slot 0x0F still holds the disassembled engine function this install
    // compares against -- REVIEW-BACKLOG APMF-B36 F2: a deny at this seat refuses a
    // slot it cannot identify). VR and any build but exactly 1.6.1170 / 1.5.97 /
    // 1.7.104 refused by name. INI [HandBlock] bHandClaimBlocksEquip (default 1)
    // turns BOTH halves (this seat and the sink step) off.
    void Install();

    // The INI switch as read at Install (true until Install says otherwise).
    bool Enabled();

    // What a live cast claim holds, for one competing equip.
    struct HandHold {
        std::uint32_t held        = 0;   // APMF_API::kEquipCat_Right / kEquipCat_Left bits held
        RE::FormID    spellR      = 0;   // the right-hand claim's named spell (0 = deny-only floor)
        RE::FormID    spellL      = 0;
        bool          denyOnlyR   = false;
        bool          denyOnlyL   = false;
        bool          spellsOnlyR = false;   // the right-hand floor carries kCastFlag_FloorSpellsOnly
        bool          spellsOnlyL = false;
    };

    // Is `a_item` a SPELL-LIKE equip for the spells-only floor: a spell, a scroll, or a
    // staff (a weapon that casts)? Anything else (a weapon, a shield, a torch, unarmed,
    // null) is not. A form-type read and TESObjectWEAP::IsStaff only, any thread.
    bool SpellLike(const RE::TESForm* a_item);

    // ANY THREAD (combat thread at 0x0F, the equip thread at the sink). Lock-free RCU
    // reads only, no form lookup. Of the hands in `a_competes` (EquipCategory bits;
    // only kEquipCat_Right / kEquipCat_Left are read), which are held by a live
    // kIntent_Cast claim on `a_actor`? The claim's OWN spell or proxy (`a_item`) is
    // never refused: it is the action that holds the hand, not another one. A
    // spells-only floor (kCastFlag_DenyHandOnly | kCastFlag_FloorSpellsOnly) holds its
    // hand only when `a_spellLike` (SpellLike of the competing item) is true.
    // False (and `a_out.held == 0`) when the switch is off or nothing is held.
    bool HeldFor(RE::FormID a_actor, std::uint32_t a_competes, RE::FormID a_item, bool a_spellLike, HandHold& a_out);

    // ANY THREAD. Cheap pre-gate for the sink: does any live cast claim hold a hand
    // (a spells-only floor included: HeldFor then decides per item)?
    bool AnyHandHeld(RE::FormID a_actor);

    // Log text for one hand of a HandHold: "-", "spells-only floor", "deny-only floor" or
    // "claim (spell 0x...)" -- says whether the refusal came from a claim or a floor.
    std::string HoldDesc(const HandHold& a_hold, std::uint32_t a_handBit);

    // Review F3 (for core/AiCastSeats.cpp's Ranged probe, chained OUTSIDE this seat): is `a_fn`
    // this file's 0x0F thunk? (the probe's install line names it instead of "a prior hook").
    bool IsThunk(std::uintptr_t a_fn);
    // SAME THREAD, right after a CheckShouldEquip call through the chain returned
    // `a_chainAnswer` for `a_item`: the ENGINE's own answer beneath this seat, and in
    // `a_refused` whether this seat turned it to NO. A call that never reached this seat
    // returns `a_chainAnswer` with `a_refused == false`.
    bool EngineAnswerFor(const void* a_item, bool a_chainAnswer, bool& a_refused);

    // "R", "L", "R+L" or "-" for a held mask (log text).
    const char* HeldName(std::uint32_t a_held);
}
