#pragma once

#include <cstdint>
#include <string_view>

namespace RE {
    using FormID = std::uint32_t;   // identical to CommonLib's alias (APMF_API.h does the same)
    class Actor;
}

// ============================================================================
// THE SUBSTRATE'S SPELL EQUIP (ch.27, feat/apmf-substrate-equip, 2026-10-19). The engine's OWN combat
// AI puts the claimed spell in the claimed hand while a substrate holds the actor: Harbinger fires no
// equip. This file names the two behaviour-tree nodes the allow-list must let run for that (while a
// DRIVING cast claim stands, exactly like the cast leaves) and logs the window. See SubstrateEquip.cpp
// for the disassembly that proves the path (both runtimes) and why these two nodes and no other.
//
// Threads: Allows / NoteAllowed run on COMBAT threads (channels/CombatSubstrate.cpp's ForLeaf, called by
// core/ActionGate.cpp's act() thunk): no lock, no engine call, no form lookup, the node's own member
// and this file's atomics only. Begin / Serve / Closing / Reset run on the GAME THREAD from
// combatsubstrate's Poll seat (and Install / ResetAll).
// ============================================================================

namespace apmf::substrateequip {

    // The holder slots this file can track. channels/CombatSubstrate.cpp static_asserts its kSlots <= this.
    inline constexpr int kMaxSlots = 32;

    enum class Node : std::uint8_t {
        kNone           = 0,
        kEquipSpell     = 1,   // CombatBehaviorEquipSpell ("Equip magic"): equips the equip context's spell item
        kActionResource = 2,   // CombatBehaviorAcquireResource<2>: runs ONLY for its "Action" instance
    };

    // combatsubstrate::Install's leaf classification (game thread): the equip-path node a catalog name is,
    // or kNone. Reads [CombatSubstrate] bSubstrateEquip on its first call; with 0 every name is kNone (the
    // two nodes stay denied, as before this file).
    Node        Classify(std::string_view a_leafName);
    const char* NodeName(Node a_kind);
    bool        Enabled();

    // ForLeaf (COMBAT THREAD), only for a live holder with a live DRIVING cast claim: true = this node runs.
    // kEquipSpell: always. kActionResource: only when the node's own resource name (node+0x28, a
    // BSFixedString) is "Action"; the "Movement" instance (and anything else) stays denied.
    bool Allows(Node a_kind, const void* a_node);
    // ForLeaf (COMBAT THREAD), after Allows said yes: count it against the holder's slot (no log here).
    void NoteAllowed(int a_slot, RE::FormID a_actor, Node a_kind);

    // GAME THREAD call-outs from channels/CombatSubstrate.cpp (one line each):
    // TakeSlot succeeded: a fresh window for this slot.
    void Begin(int a_slot, RE::FormID a_actor, std::uint64_t a_now);
    // Serve, while the holder's DRIVING cast claim stands: logs "EQUIP allowed" once per window per node and
    // samples whether the claimed form is in the claimed hand. `a_flags` = the claim's kCastFlag_* word.
    void Serve(int a_slot, RE::Actor* a_actor, RE::FormID a_spell, RE::FormID a_proxy, std::uint32_t a_flags,
               std::uint64_t a_now);
    // CloseSub / DropSub, before the slot is left: the window's outcome line (did the spell reach the hand).
    void Closing(int a_slot, RE::FormID a_actor, RE::Actor* a_actorPtr, const char* a_how, std::uint64_t a_now);
    // ResetAll (revert / kPreLoadGame): forget every window.
    void Reset();
}
