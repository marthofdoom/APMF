#pragma once

// ============================================================================
// APMF core -- the CHANNEL interface. Each directable AI-control facet (movement,
// disposition, headtrack, casting selection, ...) is ONE Channel subclass in its
// own small file under channels/. Adding a facet = drop one file that subclasses
// Channel and self-registers (APMF_REGISTER_CHANNEL); nothing else is touched.
//
// Operating principle -- APMF IS THE GATEKEEPER (marth 2026-09-02, design.md
// Section 1a). Once APMF owns a channel on an actor, nothing else reaches that
// channel except through APMF. A channel's job is to BLOCK the foreign input at
// its source so nothing competes -- deny the losing source, or set the input the
// AI itself reads. It does NOT let the AI produce a write and then override it
// every frame: a re-assert loop is a FAILED block, a symptom of not-blocking, not
// an acceptable pattern. A channel that still needs re-assert (because we have not
// yet blocked the AI's write) is a KNOWN-INCOMPLETE block: it says so in its
// header and overrides Tick() as a flagged stopgap. Every complete channel leaves
// Tick() empty.
// ============================================================================

namespace apmf {

    // A hotkey the test surface routes to a channel (DirectInput scancode + a
    // human label for the startup help log). Test-surface only; the real driver
    // is the client API (core/ClientAPI.h).
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

        // Test surface: keys this channel listens for (empty = none).
        virtual std::span<const Hotkey> Hotkeys() const { return {}; }

        // A registered hotkey fired. `target` is the arbiter's gated actor (may be
        // null -> the channel should refuse and log). Toggle engage/deny here.
        virtual void OnHotkey(std::uint32_t code, RE::Actor* target) = 0;

        // Per-tick drive for the engaged target, from the central 0xAD hook on the
        // main/sim thread. Default: NOTHING -- a real block needs no tick. Only a
        // KNOWN-INCOMPLETE block (the AI write is not yet blocked) overrides this,
        // as a flagged stopgap.
        virtual void Tick(RE::Actor* /*actor*/) {}

        // Clean release (disengage, target-unload, kPreLoadGame). Restore any input
        // the channel changed so the AI resumes ownership.
        virtual void Release(RE::Actor* /*actor*/) {}

        bool Engaged() const { return engaged.load(std::memory_order_relaxed); }

    protected:
        // Input events and the 0xAD tick both run on the main thread in this
        // runtime; the flag is atomic as cheap defensive insurance (mirrors the
        // prototype), never as a lock.
        std::atomic<bool> engaged{ false };
    };

}
