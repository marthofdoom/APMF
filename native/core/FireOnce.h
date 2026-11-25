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
// never asked for (the AI's own, begun before the claim, or a client's direct cast)
// is not the claim's fire. Spent is FINAL for that handle.
// ============================================================================
namespace apmf::fireonce {

    using Handle = std::uint32_t;   // APMF_API::Handle
    using Form   = std::uint32_t;

    class Ledger {
    public:
        // The seats answered YES for `a_h`. True when this is the claim's first YES.
        bool NoteYes(Handle a_h, Form a_actor, Form a_driven) {
            return m_recs.try_emplace(a_h, Rec{ a_actor, a_driven, false }).second;
        }

        bool Spent(Handle a_h) const {
            const auto it = m_recs.find(a_h);
            return it != m_recs.end() && it->second.spent;
        }

        // `a_actor` cast `a_form`: spends every unspent record of that pair and returns their
        // handles (more than one only when an older claim of the pair was released unfired).
        std::vector<Handle> NoteCast(Form a_actor, Form a_form) {
            std::vector<Handle> out;
            for (auto& [h, r] : m_recs)
                if (!r.spent && r.actor == a_actor && r.driven == a_form) {
                    r.spent = true;
                    out.push_back(h);
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
            Form actor  = 0;
            Form driven = 0;
            bool spent  = false;
        };
        std::unordered_map<Handle, Rec> m_recs;
    };

    // The contract, run once at install (a failure refuses the flag by name) and by the host test.
    inline bool SelfCheck() {
        Ledger l;
        if (l.Spent(1) || !l.NoteCast(10, 20).empty()) return false;               // no YES: nothing to spend
        if (!l.NoteYes(1, 10, 20) || l.NoteYes(1, 10, 20) || l.Spent(1)) return false;   // a YES alone is no fire
        if (!l.NoteCast(11, 20).empty() || !l.NoteCast(10, 21).empty()) return false;    // other actor / other form
        const auto f = l.NoteCast(10, 20);
        if (f.size() != 1 || f[0] != 1 || !l.Spent(1)) return false;               // the fire
        if (l.NoteYes(1, 10, 20) || !l.Spent(1)) return false;                     // never re-armed
        if (!l.NoteCast(10, 20).empty()) return false;                             // counted once
        if (!l.NoteYes(2, 10, 20) || l.Spent(2)) return false;                     // a new claim, same spell: its own count
        if (l.NoteCast(10, 20).size() != 1 || !l.Spent(2)) return false;
        l.Erase(1);
        if (l.Spent(1) || !l.Spent(2) || l.Size() != 1) return false;
        l.Clear();
        return l.Size() == 0 && !l.Spent(2);
    }

}
