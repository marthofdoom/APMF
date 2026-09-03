#pragma once

// ============================================================================
// SHARED multi-actor claim set for the T1 + 0x49 probes (Docs/PROBE-ALLOWANCE.md).
// Generalizes the old single-`FormID` claim to a small SET so marth can claim
// every combatant in a fight at once (visual proof the T1 Attack-leaf deny
// actually stops a whole battle from landing hits, not just one NPC).
//
// LOCK-FREE (INVARIANTS #17: no mutex in a hot thunk) -- single-writer-in-
// practice (the input/hotkey thread is the only writer; T1's combat-thread
// thunk and the 0x49 thunk are READ-ONLY callers of Contains()), multi-reader.
// A fixed-capacity flat array of `atomic<FormID>` slots (0 = empty); every op
// touches only individual slots via relaxed atomics -- no lock, no dynamic
// allocation, bounded (a battle-sized claim set, not an unbounded one). A
// benign race between two near-simultaneous writers (e.g. Add and Remove of
// the same id from two threads) can leave a slot transiently inconsistent for
// one read, never a torn/garbage FormID and never a crash -- acceptable for a
// throwaway instrumentation probe.
// ============================================================================

namespace apmf::probeclaim {

    inline constexpr std::size_t kCap = 64;

    // Add `id` if not already present and capacity allows. Returns true if it
    // was actually added (false if already present, or the set is full --
    // callers should log a capacity warning on false + !Contains(id)).
    bool Add(RE::FormID id);

    // Remove `id` if present. Returns true if it was present and removed.
    bool Remove(RE::FormID id);

    // Toggle membership: remove if present, else add. Returns true if `id` is
    // now PRESENT (i.e. it was just added), false if now ABSENT (just removed
    // or the set was full and it was never added).
    bool Toggle(RE::FormID id);

    bool Contains(RE::FormID id);

    // Empty the whole set (release-all). Cheap, idempotent, safe to call from
    // multiple probes' ClearOnPreLoad -- always leaves the set empty regardless
    // of how many times it's called.
    void Clear();

    std::size_t Count();

}
