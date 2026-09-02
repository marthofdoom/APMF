#pragma once

// ============================================================================
// APMF core -- the ARBITER. The single decision point that owns "which actor is
// under APMF control right now" and drives the engaged channels each tick.
//
// Layer 1 (design.md Section 2): the central Actor::Update(0xAD) hook calls
// OnActorUpdate for EVERY NPC; the arbiter acts only on the gated target. For the
// test surface the gated target is a single crosshair-picked actor; the client
// API (core/ClientAPI.h) will later drive engage/release per (actor, channel)
// through this same arbiter, so a request from a client and the hotkey test
// surface converge on one path.
//
// The arbiter never substitutes the package (design.md Section 5). It holds the
// package identity captured at engage and logs PACKAGE STABLE each ~second so a
// coherence regression is visible.
// ============================================================================

namespace apmf {

    class Arbiter {
    public:
        static Arbiter& Get();

        // Resolve the gated target: while any channel is engaged, keep the current
        // target (ignore the crosshair -- no mid-session hijack); when idle,
        // crosshair-pick and capture package identity. May return null (nothing
        // aimed at) -- callers refuse and log.
        RE::Actor* EnsureTarget();
        RE::Actor* CurrentTarget() const { return target.load(std::memory_order_relaxed); }
        RE::FormID PackageAtCapture() const { return pkgAtCapture; }

        // Drop the gated target if no channel is engaged.
        void ClearTargetIfIdle();

        // From the 0xAD hook, for EVERY actor. Ticks engaged channels on the gated
        // target and logs observability.
        void OnActorUpdate(RE::Actor* actor);

        // Route a hotkey to the channel that owns it (test surface).
        void DispatchHotkey(std::uint32_t code);

        // Release every engaged channel and clear the target.
        void ReleaseAll(const char* why);

    private:
        std::atomic<RE::Actor*>    target{ nullptr };   // fast identity compare in the hot thunk
        RE::ActorHandle            handle{};            // liveness source of truth
        RE::FormID                 pkgAtCapture{ 0 };   // package held at capture (coherence check)
        std::atomic<std::uint64_t> ticks{ 0 };
    };

}
