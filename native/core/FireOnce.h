#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// ============================================================================
// ch.8b FIRE ONCE (ABI v47, kCastFlag_FireOnce): the per-claim fire ledger.
//
// PURE LOGIC, std only, no lock and no engine type, so tools/fire_once/fire_once_test.cpp
// runs exactly this code. core/CastSeats.cpp owns the one instance and its leaf mutex.
//
// One record per claim HANDLE, created by the first 0x06 YES the seats gave that claim.
// Handles come from one counter that never repeats in a process, so a record can never
// name a newer claim. A cast event (the caster's TESSpellCastEvent for the driven form)
// spends the record. An event with no YES on record spends nothing: a cast that claim
// never asked for (the AI's own, or one begun before the claim) is not the claim's
// fire (CastSpellImmediate raises no event at all). Spent is FINAL for that handle.
// Every time is the caller's monotonic clock in ms.
// ============================================================================
namespace apmf::fireonce {

    using Handle = std::uint32_t;   // APMF_API::Handle
    using Form   = std::uint32_t;

    // One record a cast event spent.
    struct Match {
        Handle        handle    = 0;
        std::uint64_t lastYesMs = 0;   // the claim's LAST start seat YES
    };

    class Ledger {
    public:
        // The seats answered YES for `a_h` at `a_nowMs`. True when this is the claim's first YES.
        bool NoteYes(Handle a_h, Form a_actor, Form a_driven, std::uint64_t a_nowMs) {
            const auto [it, first] = m_recs.try_emplace(a_h, Rec{ a_actor, a_driven, a_nowMs, false });
            if (!first && it->second.lastYesMs < a_nowMs) it->second.lastYesMs = a_nowMs;
            return first;
        }

        bool Spent(Handle a_h) const {
            const auto it = m_recs.find(a_h);
            return it != m_recs.end() && it->second.spent;
        }

        // `a_actor` cast `a_form`: spends every unspent record of that pair and returns them
        // (more than one only when an older claim of the pair was released unfired). ToRecord picks the fires.
        std::vector<Match> NoteCast(Form a_actor, Form a_form) {
            std::vector<Match> out;
            for (auto& [h, r] : m_recs)
                if (!r.spent && r.actor == a_actor && r.driven == a_form) {
                    r.spent = true;
                    out.push_back({ h, r.lastYesMs });
                }
            return out;
        }

        std::vector<Handle> Handles() const {
            std::vector<Handle> out;
            out.reserve(m_recs.size());
            for (const auto& [h, r] : m_recs) out.push_back(h);
            return out;
        }

        void        Erase(Handle a_h) { m_recs.erase(a_h); }
        void        Clear() { m_recs.clear(); }
        std::size_t Size() const { return m_recs.size(); }

    private:
        struct Rec {
            Form          actor     = 0;
            Form          driven    = 0;
            std::uint64_t lastYesMs = 0;
            bool          spent     = false;
        };
        std::unordered_map<Handle, Rec> m_recs;
    };

    // Which of one cast's spent records are the claim's FIRE. Live claims: every one. With no live claim, the NEWEST
    // handle alone, and only when its last YES is within `a_endedMaxMs`: the claim whose deadline passed (or that was
    // released) with its cast already in the hand. An older ended claim's cast is not this one. `a_live(handle)`.
    template <class LiveFn>
    std::vector<Handle> ToRecord(const std::vector<Match>& a_matches, LiveFn a_live, std::uint64_t a_nowMs,
                                 std::uint64_t a_endedMaxMs, bool& a_anyLive) {
        std::vector<Handle> out;
        const Match*        newest = nullptr;
        for (const auto& m : a_matches) {
            if (a_live(m.handle)) out.push_back(m.handle);
            if (!newest || m.handle > newest->handle) newest = &m;
        }
        a_anyLive = !out.empty();
        if (out.empty() && newest && a_nowMs <= newest->lastYesMs + a_endedMaxMs) out.push_back(newest->handle);
        return out;
    }

    // The contract, run once at install (a failure refuses the flag by name) and by the host test.
    inline bool SelfCheck() {
        Ledger l;
        if (l.Spent(1) || !l.NoteCast(10, 20).empty()) return false;               // no YES: nothing to spend
        if (!l.NoteYes(1, 10, 20, 100) || l.NoteYes(1, 10, 20, 200) || l.Spent(1)) return false;   // a YES alone is no fire
        if (!l.NoteCast(11, 20).empty() || !l.NoteCast(10, 21).empty()) return false;    // other actor / other form
        const auto f = l.NoteCast(10, 20);
        if (f.size() != 1 || f[0].handle != 1 || f[0].lastYesMs != 200 || !l.Spent(1)) return false;               // the fire
        if (l.NoteYes(1, 10, 20, 300) || !l.Spent(1)) return false;                     // never re-armed
        if (!l.NoteCast(10, 20).empty()) return false;                             // counted once
        if (!l.NoteYes(2, 10, 20, 400) || l.Spent(2)) return false;                     // a new claim, same spell: its own count
        if (l.NoteCast(10, 20).size() != 1 || !l.Spent(2)) return false;
        l.Erase(1);
        if (l.Spent(1) || !l.Spent(2) || l.Size() != 1) return false;
        l.Clear();
        return l.Size() == 0 && !l.Spent(2);
    }

}
