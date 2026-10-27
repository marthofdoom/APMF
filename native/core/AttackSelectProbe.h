#pragma once

// ============================================================================
// core/AttackSelectProbe -- [Probe] bAttackSelect: the PASSIVE attack-selection probe
// (ClickUp 86e3bw775, the combat-behaviour facet's design pass, 2026-10-27; principle 5).
// See AttackSelectProbe.cpp's header for what the engine does at each point it reads.
//
// It logs, per combatant, which of the combat behaviour tree's attack-selection points ran
// and what they chose: the hand / bash context the tree built (and how many attack entries
// that context gathered), the offence / defence / ranged / magic leaf it entered, whether
// that leaf started, and which attack entry (ATKE event, power / bash flag) the engine's
// pick left on the actor. It never denies, changes or delays anything: every call runs the
// original with the original arguments, and the reads are the ones the engine itself makes
// at the same point.
//
// Default OFF. With bAttackSelect=0 nothing is installed and the ch.7 thunk pays one
// relaxed load.
// ============================================================================

namespace apmf::atkprobe {

    // kDataLoaded, MAIN thread, after actiongate::Install(). Reads [Probe] bAttackSelect and
    // iAttackSelectScope. VR- and exact-runtime-gated (1.6.1170 / 1.5.97 / 1.7.104). Installs
    // the two context-node seats (slot 0x02 of the melee and block CreateContextNode2
    // vtables, SeatVerified rows CtxNode.Melee / CtxNode.Block) and arms the leaf half, which
    // rides ch.7's existing act() thunk on the 70 leaves (no seat of its own).
    void Install();

    // Lock-free: the leaf half is armed (acquire; pairs with Install's release).
    bool Armed();

    // The watched leaf index for this leaf vtable, or -1. Read-only table after Install.
    int LeafIndex(std::uintptr_t a_vtable);

    // What BeforeAct captured for AfterAct. Opaque to the caller.
    struct ActPre
    {
        RE::NiPointer<RE::Actor> actor;
        const void*              pickBefore = nullptr;
        RE::FormID               fid        = 0;
        int                      leaf       = -1;
    };

    // ch.7's ActThunk calls these around its own body for a watched leaf while Armed().
    // ANY combat thread. No log, no form lookup, one short leaf lock in AfterAct.
    void BeforeAct(int a_leaf, void* a_control, ActPre& a_pre);
    void AfterAct(const ActPre& a_pre, void* a_node, void* a_control);

    // Arbiter::OncePerFrame, MAIN thread: drains the records into log lines (rate limited,
    // capped per session) and prints per-actor summaries (~20 s) and a RULE C totals line
    // (~30 s, zeros included). One relaxed load when not armed.
    void Heartbeat();

}
