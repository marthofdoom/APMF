#pragma once

// ============================================================================
// LOCAL RE:: EXTENSION -- the combat behavior-tree classes CommonLibSSE-NG
// does NOT ship (verified: neither the colorglass-pinned commit
// `c4ab853d095e81e3390b282d7ba01ab2f24ebf25` this project actually builds
// against, nor CharmedBaryon/CommonLibSSE-NG main/sync/upstream-* branches,
// contain a CombatBehaviorTreeNode.h/CombatBehaviorThread.h/
// CombatBehaviorTreeControl.h). This mirrors CombatPathingRevolution's own
// `src/RE/CombatBehaviorTreeNode.h` / `CombatBehaviorTreeControl.h` -- the
// SAME precedent Docs/ALLOWANCE-TEMPLATE.md §2 item 1 cites -- which itself
// declares these classes locally against `alandtse/CommonLibSSE-NG` (the
// build CPR compiles against). All VariantID triples below are copied
// VERBATIM from that fork's `include/RE/Offsets_VTABLE.h` /
// `Offsets_RTTI.h` at commit `3f9fc679347c99d6171b459c91db3fe2261368a7`
// (CPR's pinned submodule) -- not re-typed by hand, not guessed. Every
// number here independently cross-checked against ALLOWANCE-TEMPLATE.md's
// own cited evidence (Attack/Block/CastImmediateSpell SE+AE ids match
// exactly -- see the probe doc's "SE/AE label note" for the one
// discrepancy found: the doc's inline "SE X / AE Y" prose has the two
// labels SWAPPED relative to the header's own positional convention
// (REL::VariantID's constructor is literally named
// `VariantID(a_seID, a_aeID, a_vrOffset)` in REL/Relocation.h -- verified
// by reading the ctor signature itself, 3-for-3 against RelocationID and
// VariantOffset too). The NUMBERS match; only the doc's English labels for
// which number is SE vs AE were transposed. This header uses the raw
// triples verbatim (source order preserved) so that transposition cannot
// leak into the actual hook -- REL::VariantID resolves seID/aeID positionally
// regardless of what any comment calls them.
//
// SE/AE ONLY (matches the VariantID triples' 3rd/VR slot, which IS used --
// see core/ActionGate.cpp's VR refusal (the graduated T1 channel; formerly
// T1Probe.cpp, removed 2026-09-03), independent of this header).
// ============================================================================

#include "REL/Relocation.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace apmf::cbt {

    // ---- CombatBehaviorTreeNode (CPR: src/RE/CombatBehaviorTreeNode.h) ----
    // Base class of all ~70 leaf/selector nodes. Concrete vtable layout
    // (0-indexed, matches CPR's declared virtual order exactly):
    //   0 destroy(char)              1 get_name() -> char*
    //   2 act(CombatBehaviorTreeControl*) -> CombatBehaviorTreeControl*   <-- "Enter" (ALLOWANCE-TEMPLATE §3 slot 0x02)
    //   3 pop(control)                4 on_childfailed(control)
    //   5 on_interrupted(control)     6 SaveGame(control, buffer)
    //   7 LoadGame(control, buffer)   8 __unk_8(control) -> bool
    //   9 __unk_9() -> BSFixedString*
    // sizeof == 0x28: name(BSFixedString,0x10) parent(0x10) childs(0x18)
    // childs_count(0x20) pad(0x24).
    inline constexpr REL::VariantID RTTI_CombatBehaviorTreeNode(686393, 394204, 0x1efcf30);

    using Act_t = void* (*)(void* a_this, void* a_control);   // slot 2, type-erased -- T1Probe casts precisely

    // ---- CombatBehaviorTreeControl (CPR: src/RE/CombatBehaviorTreeControl.h) ----
    // Passed BY the engine as `act()`'s only argument (CPR calls this class
    // "control"; CommonLib's own [unshipped] internal name is
    // "CombatBehaviorThread" -- ALLOWANCE-TEMPLATE.md §5's "two REs disagree"
    // ambiguity is about THIS: is the object at this pointer a
    // CombatBehaviorController* one hop removed, or is +0x158 already the
    // CombatController*? CPR's own struct settles it structurally --
    // `master_controller` IS typed `CombatController*` directly at 0x158,
    // no extra +0x20 hop -- but T1Probe.cpp still logs BOTH interpretations
    // at runtime per the brief, since CPR's typing is not itself proof the
    // bytes there are live on every game version.
    struct TreeControl {
        char             pad000[0x158];
        void*            master_controller;   // 0x158 -- CombatController* per CPR (cross-checked below)
    };
    static_assert(offsetof(TreeControl, master_controller) == 0x158);

    // ---- CombatController (REAL upstream RE::CombatController,
    // include/RE/C/CombatController.h, pinned commit c4ab853d) --
    // attackerHandle at 0x28, safely < 0x68 (CLAUDE.md's AE +8 spin-lock
    // layout-bug boundary -- RE::CombatController itself #ifdef-guards the
    // AE-only aimControllerLock spinlock AFTER 0x68, so 0x28 is identical on
    // SE and AE). Re-declared minimally here (not the full class) since
    // pulling RE::CombatController's real header would require its sibling
    // CombatGroup/CombatState/CombatBlackboard/CombatBehaviorController/
    // CombatInventory forward-decls this probe has no other use for.
    struct ControllerMini {
        void*            combatGroup;         // 0x00
        void*            state;               // 0x08
        void*            inventory;           // 0x10
        void*            blackboard;           // 0x18
        void*            behaviorController;   // 0x20 -- the CPR-hypothesis hop (see TreeControl comment)
        RE::ActorHandle  attackerHandle;        // 0x28
    };
    static_assert(offsetof(ControllerMini, attackerHandle) == 0x28);
    static_assert(offsetof(ControllerMini, attackerHandle) < 0x68);

    struct LeafEntry {
        const char*      name;
        REL::VariantID   vtbl;
    };

    // All 70 leaf VTABLE_CombatBehaviorTreeNodeObject_* symbols (verbatim
    // triples, see file header). Alphabetical (matches Offsets_VTABLE.h
    // symbol sort, not tree-declaration order -- irrelevant, this is a flat
    // install list).
    inline constexpr std::array<LeafEntry, 70> kLeaves{ {
        { "CombatBehaviorAdvance", REL::VariantID(266096, 212694, 0x17191b0) },
        { "CombatBehaviorAttack", REL::VariantID(266747, 213789, 0x17216a0) },
        { "CombatBehaviorAttackFromCover", REL::VariantID(267194, 214395, 0x17268a8) },
        { "CombatBehaviorAttackLow", REL::VariantID(266654, 213640, 0x1720310) },
        { "CombatBehaviorBackoff", REL::VariantID(266103, 212785, 0x1719680) },
        { "CombatBehaviorBash", REL::VariantID(265986, 212595, 0x1717ed8) },
        { "CombatBehaviorBlock", REL::VariantID(265987, 212608, 0x1717f88) },
        { "CombatBehaviorBlockAttack", REL::VariantID(265985, 212582, 0x1717e28) },
        { "CombatBehaviorCastConcentrationSpell", REL::VariantID(266707, 213735, 0x1720e98) },
        { "CombatBehaviorCastImmediateSpell", REL::VariantID(266705, 213720, 0x1720d90) },
        { "CombatBehaviorCastShout", REL::VariantID(267116, 214280, 0x17259e0) },
        { "CombatBehaviorChase", REL::VariantID(266336, 213109, 0x171c2f0) },
        { "CombatBehaviorCheckUnreachableTarget", REL::VariantID(266866, 213917, 0x1722ac8) },
        { "CombatBehaviorChildSelector_ConditionalChildSelector_", REL::VariantID(266094, 212668, 0x1719050) },
        { "CombatBehaviorChildSelector_RandomValueChildSelector_", REL::VariantID(266100, 212746, 0x1719470) },
        { "CombatBehaviorChildSelector_ValueChildSelector_", REL::VariantID(265919, 212424, 0x1716eb0) },
        { "CombatBehaviorCircle", REL::VariantID(266102, 212772, 0x17195d0) },
        { "CombatBehaviorCircleDistant", REL::VariantID(266098, 212720, 0x1719310) },
        { "CombatBehaviorDiveBomb", REL::VariantID(266624, 213551, 0x171fa30) },
        { "CombatBehaviorDodgeThreat", REL::VariantID(265954, 212546, 0x17178b8) },
        { "CombatBehaviorDrinkPotion", REL::VariantID(267213, 214430, 0x1726c40) },
        { "CombatBehaviorDynamicConditionalNode", REL::VariantID(265918, 212411, 0x1716e00) },
        { "CombatBehaviorEquipObject", REL::VariantID(265920, 212437, 0x1716f60) },
        { "CombatBehaviorEquipRangedWeapon", REL::VariantID(265921, 212450, 0x1717010) },
        { "CombatBehaviorEquipShout", REL::VariantID(265924, 212478, 0x17171c8) },
        { "CombatBehaviorEquipSpell", REL::VariantID(265922, 212463, 0x17170c0) },
        { "CombatBehaviorExitWater", REL::VariantID(266868, 213943, 0x1722c28) },
        { "CombatBehaviorFallback", REL::VariantID(266101, 212759, 0x1719520) },
        { "CombatBehaviorFallbackSelector_NextChildSelector_", REL::VariantID(256698, 205291, 0x16a6190) },
        { "CombatBehaviorFallbackSelector_WeightedRandomChildSelector_", REL::VariantID(266621, 213512, 0x171f820) },
        { "CombatBehaviorFallbackToRanged", REL::VariantID(266095, 212681, 0x1719100) },
        { "CombatBehaviorFindAllyAttackLocation", REL::VariantID(266196, 212925, 0x171a9c0) },
        { "CombatBehaviorFindAttackLocation", REL::VariantID(266195, 212912, 0x171a910) },
        { "CombatBehaviorFindCover", REL::VariantID(267190, 214365, 0x1726698) },
        { "CombatBehaviorFindLateralAttackLocation", REL::VariantID(266194, 212899, 0x171a860) },
        { "CombatBehaviorFindWeapon", REL::VariantID(265838, 212262, 0x1716060) },
        { "CombatBehaviorFlank", REL::VariantID(266335, 213096, 0x171c240) },
        { "CombatBehaviorFlankDistant", REL::VariantID(266337, 213122, 0x171c3a0) },
        { "CombatBehaviorFlee", REL::VariantID(266497, 213321, 0x171dfb8) },
        { "CombatBehaviorFleeThroughDoor", REL::VariantID(266495, 213295, 0x171de58) },
        { "CombatBehaviorFleeToAlly", REL::VariantID(266494, 213282, 0x171dda8) },
        { "CombatBehaviorFleeToCover", REL::VariantID(266496, 213308, 0x171df08) },
        { "CombatBehaviorFlyingAttack", REL::VariantID(266626, 213577, 0x171fb90) },
        { "CombatBehaviorForceFail", REL::VariantID(267085, 214228, 0x17253a0) },
        { "CombatBehaviorForceSuccess", REL::VariantID(266506, 213405, 0x171e4e0) },
        { "CombatBehaviorGroundAttack", REL::VariantID(266622, 213525, 0x171f8d0) },
        { "CombatBehaviorHide", REL::VariantID(266502, 213364, 0x171e278) },
        { "CombatBehaviorHover", REL::VariantID(266623, 213538, 0x171f980) },
        { "CombatBehaviorLand", REL::VariantID(266631, 213609, 0x171fdf8) },
        { "CombatBehaviorMaintainOptimalRange", REL::VariantID(266933, 214013, 0x1723730) },
        { "CombatBehaviorOrbit", REL::VariantID(266620, 213499, 0x171f770) },
        { "CombatBehaviorOrbitDistant", REL::VariantID(266619, 213486, 0x171f6c0) },
        { "CombatBehaviorParallel", REL::VariantID(265772, 212197, 0x1715568) },
        { "CombatBehaviorPause", REL::VariantID(265925, 212491, 0x1717278) },
        { "CombatBehaviorPerchAttack", REL::VariantID(266625, 213564, 0x171fae0) },
        { "CombatBehaviorPrepareDualCast", REL::VariantID(266703, 213705, 0x1720c88) },
        { "CombatBehaviorPursueTarget", REL::VariantID(266655, 213653, 0x17203c0) },
        { "CombatBehaviorRangedAttack", REL::VariantID(256699, 205304, 0x16a6240) },
        { "CombatBehaviorRepeat", REL::VariantID(256697, 205278, 0x16a60e0) },
        { "CombatBehaviorReposition", REL::VariantID(266099, 212733, 0x17193c0) },
        { "CombatBehaviorReturnToCombatArea", REL::VariantID(266867, 213930, 0x1722b78) },
        { "CombatBehaviorSearchInvestigateDoor", REL::VariantID(267086, 214241, 0x1725450) },
        { "CombatBehaviorSequence", REL::VariantID(265837, 212249, 0x1715fb0) },
        { "CombatBehaviorSpecialAttack", REL::VariantID(266746, 213776, 0x17215f0) },
        { "CombatBehaviorStalk", REL::VariantID(266334, 213083, 0x171c190) },
        { "CombatBehaviorStrafe", REL::VariantID(266934, 214026, 0x17237e0) },
        { "CombatBehaviorSurround", REL::VariantID(266097, 212707, 0x1719260) },
        { "CombatBehaviorTakeoff", REL::VariantID(266617, 213460, 0x171f560) },
        { "CombatBehaviorTrackTarget", REL::VariantID(266500, 213338, 0x171e118) },
        { "CombatBehaviorWaitBehindCover", REL::VariantID(267193, 214382, 0x17267f8) },
    } };

    // ---- Cast/equip CONTEXT-CREATION nodes (deny-completeness, 2026-09-04) ----
    // These are NOT leaves. They are `CombatBehaviorTreeCreateContextNode*`
    // nodes whose act() (slot 0x02, SAME base-class vtable layout as every
    // leaf -- they all derive CombatBehaviorTreeNode) BUILDS the AI's magic
    // cast/equip CONTEXT: it selects the spell AND constructs the
    // CombatBehaviorEquipContext holding a `NiPointer<CombatInventoryItem>`,
    // then dereferences that item. The 70-leaf cast deny (kCombatActionCat_Cast)
    // stops the cast-FIRING leaves but NOT this SETUP node -- so with a
    // kIntent_Cast claim held, the AI still BUILT its magic-equip context and
    // raced the client's forced equip: EXCEPTION_ACCESS_VIOLATION `call
    // [rax+0x28]` rax=0 (null CombatInventoryItem vfunc) inside
    // CombatBehaviorTreeCreateContextNode1<CombatBehaviorContextMagic, ...
    // CombatBehaviorEquipContext, NiPointer<CombatInventoryItem>...> (deck CTD,
    // MFO.dll frame 5). Denying THIS node's act() (via the SAME ForceFail
    // mechanism the leaves use) means the AI never builds/derefs the magic-equip
    // context while the cast facet is held -- closing the last open path
    // (INVARIANTS #18, deny-completeness).
    //
    // Symbols verified PRESENT + ID-backed in the pinned CommonLib
    // (CharmedBaryon/CommonLibSSE-NG @ c4ab853d, Offsets_VTABLE.h:3704-3705,
    // Offsets_RTTI.h:4095-4096). Both are RTTI-verified (DerivesFrom
    // CombatBehaviorTreeNode) at install by allowance::InstallOnVtables, so a
    // symbol that does NOT actually derive the node base is SKIPPED, never
    // hooked blind (INVARIANTS #17). Node1 (the concrete instantiation the live
    // object dispatches through -- the crash frame) is the load-bearing target;
    // the Base is included for completeness and skipped harmlessly if abstract.
    inline constexpr std::array<LeafEntry, 2> kCastContextNodes{ {
        { "CombatBehaviorTreeCreateContextNode1_CombatBehaviorContextMagic",
          REL::VariantID(266702, 213692, 0x1720bd8) },
        { "CombatBehaviorTreeCreateContextNodeBase_CombatBehaviorContextMagic",
          REL::VariantID(550933, 213681, 0x1720b80) },
    } };

    // Index of "CombatBehaviorAttack" within kLeaves -- the Phase-1 DENY target.
    inline int AttackIndex() {
        for (int i = 0; i < static_cast<int>(kLeaves.size()); ++i)
            if (std::string_view(kLeaves[static_cast<std::size_t>(i)].name) == "CombatBehaviorAttack") return i;
        return -1;
    }
    // Index of "CombatBehaviorForceFail" -- the deny-mechanism source (core/ActionGate.cpp).
    inline int ForceFailIndex() {
        for (int i = 0; i < static_cast<int>(kLeaves.size()); ++i)
            if (std::string_view(kLeaves[static_cast<std::size_t>(i)].name) == "CombatBehaviorForceFail") return i;
        return -1;
    }

}
