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
        //
        // Every call carries the NPC's `id` (FormID) AND its resolved `actor`
        // pointer. Key any per-NPC state map by `id`, NOT by `actor->GetFormID()`:
        // `actor` MAY BE NULL (a deleted form the ControlMap could not resolve), and
        // a channel must still be able to erase/clean its per-NPC entry then. Do
        // engine writes only when `actor` is non-null.

        // First claim landed: capture the prior engine state for restore and apply
        // the source-block. Called once per 0->1 claim transition (actor is live).
        // `param` is the WINNING claim's POD payload (APMF_API.h) telling the channel
        // WHICH thing to act on (cast-select => param.form is the spell); an all-zero
        // param means "no param" and the channel falls back to its default.
        virtual void Engage(RE::FormID id, RE::Actor* actor,
                            const APMF_API::APMF_Param& param) = 0;

        // The winning claim on an ALREADY-engaged channel changed its param (a new
        // higher-basis claim arrived, or the owner released and another claim now
        // wins). Default: NOTHING -- a parameterless channel does not care which
        // client owns it. A PARAMETERIZED channel (cast-select) overrides this to
        // switch to the new winner's payload WITHOUT re-capturing the restore state
        // (Engage already captured it; only Release restores it).
        virtual void OnOwnerChanged(RE::FormID /*id*/, RE::Actor* /*actor*/,
                                    const APMF_API::APMF_Param& /*param*/) {}

        // Per-tick drive for an engaged NPC. Default: NOTHING -- a real block needs
        // no per-tick work (INVARIANTS #1). Only a KNOWN-INCOMPLETE block overrides
        // this, as a flagged stopgap.
        virtual void Tick(RE::FormID /*id*/, RE::Actor* /*actor*/) {}

        // Last claim released (or the NPC unloaded/was deleted): restore whatever
        // Engage changed (if `actor` is non-null) and drop the per-NPC entry keyed by
        // `id` regardless. Called once per 1->0 claim transition.
        virtual void Release(RE::FormID id, RE::Actor* actor) = 0;
    };

}
