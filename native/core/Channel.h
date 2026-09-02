#pragma once
#include "APMF_API.h"

// ============================================================================
// APMF core -- the CHANNEL interface. Each directable AI-control facet (movement,
// disposition, headtrack, casting selection, ...) is ONE Channel subclass in its
// own small file under channels/. Adding a facet = drop one file that subclasses
// Channel and self-registers (APMF_REGISTER_CHANNEL); nothing else is touched.
//
// PER-NPC MODEL (Phase 1). A channel is a program-lifetime SINGLETON that operates
// on ANY number of NPCs at once. It owns NO single "engaged" flag; instead the
// ControlMap tracks which NPCs have this channel engaged and refcounts client
// claims, and calls the channel's per-NPC lifecycle (Engage/Tick/Release) keyed by
// actor. A channel that needs to remember per-NPC restore data (a prior AV, the
// prior spell) keeps its OWN small std::unordered_map<RE::FormID, State> -- touched
// ONLY on the game thread (Docs/INVARIANTS.md #12), so no lock.
//
// Operating principle -- APMF IS THE GATEKEEPER (marth 2026-09-02, design.md
// Section 1a). Once APMF owns a channel on an actor, nothing else reaches that
// facet except through APMF. A channel BLOCKS the foreign input at its source so
// nothing competes -- deny the losing source, or set the input the AI itself reads.
// It does NOT let the AI produce a write and then override it every frame: a
// re-assert loop is a FAILED block (Tick() is empty by default for exactly this
// reason). A channel that still needs re-assert is a KNOWN-INCOMPLETE block: it
// says so in its header and overrides Tick() as a flagged stopgap (INVARIANTS #2).
// ============================================================================

namespace apmf {

    // A hotkey the test surface routes to a channel (DirectInput scancode + a
    // human label for the startup help log). Test-surface only; the real driver is
    // the C-ABI client API (APMF_API.h).
    struct Hotkey {
        std::uint32_t code;
        const char*   label;
    };

    class Channel {
    public:
        virtual ~Channel() = default;

        // Identity for logs + the CHANNEL-MAP number this implements.
        virtual const char* Name() const = 0;
        virtual int         ChannelNo() const = 0;

        // The client Intent this channel serves. The ControlMap resolves a
        // Request's intent to the channel whose ServesIntent() matches. Each
        // channel serves a DISTINCT intent (the per-NPC channel key).
        virtual APMF_API::Intent ServesIntent() const = 0;

        // Test surface: keys this channel listens for (empty = none).
        virtual std::span<const Hotkey> Hotkeys() const { return {}; }

        // ---- Per-NPC lifecycle. ALL on the game thread (the ControlMap drives
        // them from the 0xAD hook / its once-per-frame drain). ----

        // First claim landed on `actor`: capture the prior engine state for restore
        // and apply the source-block. Called exactly once per 0->1 claim transition.
        virtual void Engage(RE::Actor* actor) = 0;

        // Per-tick drive for an engaged `actor`. Default: NOTHING -- a real block
        // needs no per-tick work (INVARIANTS #1). Only a KNOWN-INCOMPLETE block
        // (the AI write is not yet blocked) overrides this, as a flagged stopgap.
        virtual void Tick(RE::Actor* /*actor*/) {}

        // Last claim released on `actor` (or the actor unloaded): restore whatever
        // Engage changed and drop the per-NPC state. `actor` may be NULL if the
        // actor unloaded -- drop the internal entry regardless (a dead actor needs
        // no restore). Called exactly once per 1->0 claim transition.
        virtual void Release(RE::Actor* actor) = 0;
    };

}
