#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"
#include "core/ControlMap.h"
#include "core/MainThread.h"
#include "core/EquipSink.h"
#include "core/Clock.h"

#include <algorithm>

// ============================================================================
// Channel 17 -- EQUIP AUTHORITY (kIntent_EquipAuthority, ABI v7). The ENGINE-
// EQUIP facet taken WHOLE (marth 2026-09-15: "a command is sent to APMF with what
// to equip and that is enforced until overridden"; Docs/INVARIANTS.md #17a).
//
// THE SHAPE. A claim here is a STANDING authority over what the actor wears: no
// TTL, ended only by Release (or the losing side of arbitration). The claim
// alone changes nothing. The client then DECLARES the worn set with
// APMF_API_v7::SetEquipSet (base FormIDs, bounded by kMaxEquipSet), and from
// that declaration on the facet has two halves:
//   DENY   -- core/EquipSink.cpp, the #17a call-site seat at the one worker every
//             engine equip funnels through, refuses every engine equip of an
//             off-set item on this actor (observe-only logs `would-deny`
//             instead, and Papyrus/console pass unless kEquipAuth_DenyScript).
//   EQUIP  -- THIS file: on each applied declaration for the OWNING claim, one
//             main-thread pass equips every declared item the actor is not
//             already wearing, through ActorEquipManager::EquipObject (queued,
//             not forced, sounds on) inside the seat's TLS bracket so the seat
//             lets it through and logs it as APMF's own (`tls=1`).
// Nothing is ever UNEQUIPPED by APMF: the engine's own worker displaces whatever
// occupies a declared item's slot, the ordinary way. No re-assert loop follows
// (INVARIANTS #0): the seat is what keeps the engine from undoing the set.
//
// NOT A LOOP, EVEN FROM A TICKING CLIENT (Fable tier-3 on d1aa66b, SEV-3 #3).
// ControlMap fires OnOwnerChanged on EVERY applied declaration (a re-issued
// identical set is the sanctioned "give it back"), so a client that re-sends
// its set every tick (MFO's 133 ms ServiceFollower would) could otherwise turn
// this pass into a #0 re-assert loop built out of client ticks: a full
// GetInventory() walk per tick, and a queued-but-not-yet-applied item reading
// "not worn" on the next pass and being queued AGAIN. Two guards, per actor,
// main-thread state: (a) an IDENTICAL set re-issued within kCoalesceMs of the
// last pass is coalesced (skipped with a log line) -- a deliberate re-send after
// that window still runs; (b) an item this channel issued in the PREVIOUS pass
// that still reads not-worn is left alone for one pass (its queued equip is in
// flight), then retried. A set that cannot be worn simultaneously (a two-hander
// AND a shield) is the client's error: the engine displaces one with the other
// on every pass and the log shows it -- declare a wearable set, do not tick.
//
// WHY THIS IS LAWFUL UNDER #0 / #17. #17a condition 4 licenses exactly this
// pair: "the client's declared set is what APMF equips, and the seat is what
// keeps the engine from undoing it". The equip is the client's DECLARED input
// re-issued verbatim (declare->enforce, CLAUDE.md principle 4), never a
// selection APMF made; the deny is engine-answer-first in the only form a sink
// allows (do not call the worker). Release relinquishes (#5a): nothing to undo.
//
// ORDERING (INVARIANTS #20's publish-first rule, on the ENGAGE side). The
// declaration is applied inside ControlMap::Drain on the writer's private copy;
// ControlMap::ApplySetEquipSet calls OnOwnerChanged below while that copy is
// still unpublished. So this channel never equips from inside the callback: it
// posts ONE mainthread hop. Arbiter::OncePerFrame pumps mainthread strictly
// after Drain() returns, i.e. after Publish(), so by the time Enforce() runs the
// seat already reads the NEW set and TryGetEquipSet hands back exactly what the
// engine will be held to. Same thread, same frame, one hop later.
//
// SAVE-SAFETY (INVARIANTS #15). The equips persist in the .ess like any equip;
// the claim and its declaration are RAM-only and drop on kPreLoadGame
// (ReleaseAll). A client re-declares after a load. Nothing here is co-saved,
// nothing is stranded: an actor left wearing the declared set is exactly what
// the client asked for.
// ============================================================================

namespace {

    // Loud, bounded diagnostics: a declaration the framework could not carry out
    // is logged, never masked (CLAUDE.md principle 7). Per-cause counters so a
    // persistent condition logs once, then a count on every 100th recurrence.
    std::atomic<std::uint32_t> g_seatMissing{ 0 };
    std::atomic<std::uint32_t> g_notLoaded{ 0 };

    bool Every100(std::atomic<std::uint32_t>& counter) {
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        return n == 0 || (n % 100) == 0;
    }

    // ---- per-actor enforcement memory (MAIN THREAD ONLY: written by Enforce,
    // erased by Release; both run on the writer/main thread). ----
    constexpr std::uint64_t kCoalesceMs   = 1000;   // identical re-issue inside this window is coalesced
    constexpr std::uint64_t kPendingMaxMs = 3000;   // an issued-not-applied item older than this is retried
    struct ActorMemory {
        std::uint64_t lastPassMs  = 0;
        std::uint64_t lastSig     = 0;      // FNV-1a over (count, forms in order)
        std::uint32_t passes      = 0;
        std::uint64_t issuedAtMs  = 0;      // when `issued` was filled
        std::vector<RE::FormID> issued;     // forms EquipObject'd in the previous pass
        bool          reservedWarned = false;   // kEquipAuth_DenyUnequip warned once per actor
    };
    std::unordered_map<RE::FormID, ActorMemory> g_memory;

    std::uint64_t SetSignature(const apmf::EquipSetView& set) {
        std::uint64_t h = 1469598103934665603ull;
        auto mix = [&](std::uint32_t v) { h ^= v; h *= 1099511628211ull; };
        mix(set.count);
        for (std::uint32_t i = 0; i < set.count; ++i) mix(set.forms[i]);
        return h;
    }

    // MAIN THREAD, strictly after the publishing Drain (see the banner). Reads the
    // PUBLISHED winning declaration for `id` and equips each declared item the
    // actor is not already wearing. A claim released between the post and the
    // pump simply reads back as "no claim" and enforces nothing.
    void Enforce(RE::FormID id) {
        apmf::EquipSetView set;
        if (!apmf::ControlMap::Get().TryGetEquipSet(id, set)) return;   // claim gone, or lost arbitration
        if (set.count == 0) return;                                     // no declaration (or cleared): nothing to equip

        if (!apmf::equipsink::Installed()) {
            // Without the seat the engine would undo the equip on its next pass;
            // equipping anyway would only LOOK like it worked. Record, do nothing.
            if (Every100(g_seatMissing))
                spdlog::error("[apmf][equip-auth] actor=0x{} declaration of {} item(s) NOT enforced -- the equip "
                              "sink is not installed ([EquipAuthority] bEquipAuthority=0, VR, or site-verify "
                              "refused; see the [apmf][equip-sink] install lines). Recurrence #{}.",
                              apmf::log::Hex(id), set.count, g_seatMissing.load(std::memory_order_relaxed));
            return;
        }

        // Guard (a): coalesce an identical re-issue inside the window.
        const auto nowMs = apmf::clock::MonotonicMs();
        auto&      mem   = g_memory[id];
        const auto sig   = SetSignature(set);
        if (mem.passes > 0 && sig == mem.lastSig && nowMs - mem.lastPassMs < kCoalesceMs) {
            spdlog::info("[apmf][equip-auth] actor=0x{} identical declaration re-issued {} ms after pass #{} -- "
                         "coalesced (no equip issued; re-send after {} ms to force a pass).",
                         apmf::log::Hex(id), nowMs - mem.lastPassMs, mem.passes, kCoalesceMs);
            return;
        }

        auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
        if (!actor || !actor->Is3DLoaded()) {
            if (Every100(g_notLoaded))
                spdlog::warn("[apmf][equip-auth] actor=0x{} not loaded (actor {}, 3D {}) -- declaration of {} "
                             "item(s) recorded, equip pass skipped. Re-declare once the actor is loaded. Recurrence #{}.",
                             apmf::log::Hex(id), actor ? "resolved" : "null",
                             (actor && actor->Is3DLoaded()) ? "loaded" : "absent", set.count,
                             g_notLoaded.load(std::memory_order_relaxed));
            return;
        }
        auto* mgr = RE::ActorEquipManager::GetSingleton();
        if (!mgr) {
            spdlog::error("[apmf][equip-auth] actor=0x{} ActorEquipManager singleton is null -- equip pass skipped.",
                          apmf::log::Hex(id));
            return;
        }

        // One inventory snapshot for the whole pass (main thread; the map is a
        // local copy, nothing aliased into the engine).
        auto inv = actor->GetInventory();
        // Guard (b): what the PREVIOUS pass issued and may still be in the
        // engine's equip queue. Stale (older than kPendingMaxMs) means the queue
        // has long since drained -- retry rather than hold back forever.
        const bool pendingFresh = (nowMs - mem.issuedAtMs) < kPendingMaxMs;
        std::vector<RE::FormID> issuedNow;
        std::uint32_t equipped = 0, worn = 0, missing = 0, unresolved = 0, pending = 0;
        for (std::uint32_t i = 0; i < set.count; ++i) {
            const RE::FormID form = set.forms[i];
            if (form == 0) continue;
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(form);
            if (!obj) {
                ++unresolved;
                spdlog::warn("[apmf][equip-auth] actor=0x{} declared 0x{} is not a bound object (unknown form or not "
                             "equippable) -- skipped.", apmf::log::Hex(id), apmf::log::Hex(form));
                continue;
            }
            auto it = inv.find(obj);
            if (it == inv.end() || it->second.first <= 0) {
                ++missing;
                spdlog::warn("[apmf][equip-auth] actor=0x{} declared 0x{} '{}' is not in the actor's inventory -- "
                             "skipped (APMF never adds items; the seat still holds the declaration).",
                             apmf::log::Hex(id), apmf::log::Hex(form), obj->GetName() ? obj->GetName() : "");
                continue;
            }
            const auto& entry = it->second.second;
            if (entry && entry->IsWorn()) { ++worn; continue; }
            if (pendingFresh && std::find(mem.issued.begin(), mem.issued.end(), form) != mem.issued.end()) {
                // Issued last pass, not yet applied by the engine's queue: do not
                // queue a second copy of the same equip. Retried next pass.
                ++pending;
                continue;
            }

            {
                // The bracket is what the seat recognises as "APMF-issued": this
                // equip, and every engine re-entry beneath it, passes with tls>0.
                apmf::equipsink::ApmfEquipScope scope;
                mgr->EquipObject(actor, obj, nullptr, 1, nullptr, /*queue*/ true, /*force*/ false,
                                 /*sounds*/ true, /*applyNow*/ false);
            }
            ++equipped;
            issuedNow.push_back(form);
            spdlog::info("[apmf][equip-auth] actor=0x{} set={} items -> equip 0x{} '{}'",
                         apmf::log::Hex(id), set.count, apmf::log::Hex(form), obj->GetName() ? obj->GetName() : "");
        }
        mem.lastPassMs = nowMs;
        mem.lastSig    = sig;
        ++mem.passes;
        mem.issued     = std::move(issuedNow);
        mem.issuedAtMs = nowMs;
        spdlog::info("[apmf][equip-auth] actor=0x{} enforce pass #{}: set={} equipped={} already-worn={} "
                     "pending-from-previous-pass={} not-in-inventory={} unresolved={}",
                     apmf::log::Hex(id), mem.passes, set.count, equipped, worn, pending, missing, unresolved);
    }

    void PostEnforce(RE::FormID id) {
        apmf::mainthread::Post([id] { Enforce(id); });
    }

    // Once per actor (the channel sees the actor and the param, never the handle;
    // SEV-5 #11): a declaration-per-tick client would otherwise print this on
    // every pass. Reset by Release, so a fresh claim after a release warns again.
    void RefuseReservedBits(RE::FormID id, const APMF_API::APMF_Param& param) {
        const auto flags = static_cast<std::uint32_t>(param.ival);
        if (!(flags & APMF_API::kEquipAuth_DenyUnequip)) return;
        auto& mem = g_memory[id];
        if (mem.reservedWarned) return;
        mem.reservedWarned = true;
        spdlog::warn("[ch.17] 0x{} claim carries kEquipAuth_DenyUnequip -- RESERVED in this ABI, refused and "
                     "ignored: unequips are not seated. The rest of the claim stands. (Logged once per actor.)",
                     apmf::log::Hex(id));
    }

    class EquipAuthorityChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "equip-authority"; }
        int              ChannelNo() const override { return 17; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_EquipAuthority; }
        // No test hotkey: a claim with no declaration does nothing, and the test
        // surface has no way to declare a set. The client API is the only path.

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            RefuseReservedBits(id, param);
            spdlog::info("[ch.17] 0x{} equip authority CLAIMED (flags 0x{}) -- standing, no TTL. Nothing is "
                         "enforced until the client declares a worn set (SetEquipSet, ABI v7).",
                         apmf::log::Hex(id), apmf::log::Hex(static_cast<std::uint32_t>(param.ival), 0));
            // A fresh claim has no declaration; the hop below is a no-op today but
            // keeps Engage and OnOwnerChanged symmetric (a re-request after Release
            // that somehow carried state would be enforced, never silently held).
            PostEnforce(id);
        }

        // Fired by ControlMap for: a new winner on this channel (its own declared
        // set becomes the effective one), a Repoint of the owner (flags changed),
        // and EVERY applied SetEquipSet on the owning claim (ApplySetEquipSet).
        // Each is a declaration event -> one enforcement hop after Publish.
        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            RefuseReservedBits(id, param);
            spdlog::info("[ch.17] 0x{} equip authority declaration/owner changed (flags 0x{}) -- enforcement hop posted.",
                         apmf::log::Hex(id), apmf::log::Hex(static_cast<std::uint32_t>(param.ival), 0));
            PostEnforce(id);
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            // Relinquish (INVARIANTS #5a): the seat stops refusing the instant the
            // cleared claim publishes; nothing was unequipped, so nothing is undone.
            spdlog::info("[ch.17] 0x{} equip authority released -- the engine owns the worn set again; "
                         "nothing is unequipped.", apmf::log::Hex(id));
            g_memory.erase(id);   // main thread (Release runs inside Drain/ReleaseAll)
        }
        // No Tick: the deny lives entirely in the seat (INVARIANTS #1), the equip
        // fires once per declaration (#0: no re-assert loop).
    };

}

APMF_REGISTER_CHANNEL(EquipAuthorityChannel);
