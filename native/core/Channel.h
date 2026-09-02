#pragma once

// ============================================================================
// APMF core -- the CHANNEL interface. Each directable AI-control facet (movement,
// disposition, headtrack, casting selection, ...) is ONE Channel subclass in its
// own small file under channels/. Adding a facet = drop one file that subclasses
// Channel and self-registers (APMF_REGISTER_CHANNEL); nothing else is touched.
//
// Operating principle (design.md Section 1a): a channel GATES an INPUT at the
// source (deny the losing source, or set the input the AI itself reads). It does
// NOT force an output and re-assert it every frame. Where a facet has no clean
// source-gate (the AI co-writes the very slot we own, e.g. headtracking), a
// per-tick re-assert is a DOCUMENTED FALLBACK -- the channel says so in its
// header and overrides Tick(); every other channel leaves Tick() empty.
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
        // main/sim thread. Default: NOTHING -- a clean source-gate needs no tick.
        // Only a documented re-assert fallback overrides this.
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
