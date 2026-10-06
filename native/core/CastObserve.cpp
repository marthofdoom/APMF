#include "PCH.h"
#include "core/Log.h"
#include "core/CastObserve.h"
#include "core/CasterTypeCensus.h"
#include "core/Clock.h"
#include "core/Allowance.h"   // RuntimeSupported(): the leaf-name map resolves ch.7's ids only where ch.7 does
#include "core/ControlMap.h"
#include "core/Sightline.h"
#include "core/CombatBehaviorRE.h"   // the leaf vtables (names for the attribution line)

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

// ============================================================================
// See CastObserve.h for the design. FULLY PASSIVE: reads + logs only, never a
// write to the actor or the engine. The anim sink is a standard observe listener
// (AddAnimationGraphEventSink), NOT a hook / vtable patch -- it returns kContinue
// and touches nothing.
// ============================================================================

namespace apmf::castobserve {

    namespace {

        constexpr std::uint64_t kPollIntervalMs = 100;   // self-throttle the state poll
        constexpr std::uint64_t kEventDedupMs   = 250;   // per-actor repeat-tag suppression window

        // --- Poll state (GAME THREAD ONLY: Arbiter::OncePerFrame). No lock. ---
        std::uint64_t g_lastPollMs = 0;
        // Per-hand last-seen (state, spell) so we log only on a TRANSITION.
        struct HandState { std::uint32_t state = 0; RE::FormID spell = 0; };
        std::unordered_map<std::uint64_t, HandState> g_lastHand;   // key = (fid<<3 | sourceIdx)

        // --- Anim-event sink (OFF-THREAD ProcessEvent + game-thread registration) ---
        // A distinct sink per actor holding its FormID, so ProcessEvent never has to
        // resolve the event holder's concrete type. Registered once, kept for the
        // session (a global observe listener is harmless; never removed).
        std::mutex g_evMx;   // guards g_lastEvt (ProcessEvent is off the game thread)
        std::unordered_map<RE::FormID, std::pair<const char*, std::uint64_t>> g_lastEvt;

        // ---- WATCHED actors (interrupt attribution + channel end). Written by the
        // game-thread poll (RefreshWatch), read lock-free from any thread. A slot holds 0
        // when unused; g_watchN is a pre-gate only (a reader that sees a stale count
        // still compares every slot). ----
        constexpr std::size_t kMaxWatched = 16;
        std::array<std::atomic<RE::FormID>, kMaxWatched> g_watch{};
        std::atomic<std::uint32_t>                        g_watchN{ 0 };

        // ---- recorded facts per watched actor (ANY THREAD writers, under g_recMx;
        // a leaf lock: no engine call, no log, no allocation past the bounded map). ----
        enum class Ev : std::uint8_t { kEquip, kLeaf, kCaster, kCheckCast, kStop, kAnim };
        struct Event {
            std::uint64_t  ms     = 0;
            RE::FormID     form   = 0;     // item / spell
            std::uintptr_t vt     = 0;     // leaf vtable
            const char*    s1     = nullptr;   // literal: path / caster type / stop why
            const char*    s2     = nullptr;   // literal: verdict
            std::uint32_t  u      = 0;     // CheckCast reason
            std::int8_t    source = -1;    // CheckCast caster source
            bool           b1     = false; // caster: fired / CheckCast: by APMF
            bool           b2     = false; // caster: CheckStartCast answer
            char           tag[28]{};      // anim tag (copied)
        };
        template <std::size_t N>
        struct Ring {
            std::array<Event, N> e{};
            std::uint32_t        next = 0;
            void push(const Event& a_ev) { e[next++ % N] = a_ev; }
        };
        struct Recent {
            Ring<8> equip, leaf, caster, check, stop, anim;
            std::uint64_t lastMs = 0;
        };
        std::mutex                             g_recMx;
        std::unordered_map<RE::FormID, Recent> g_recent;   // bounded by the watch set (pruned by the poll)

        Ring<8>* RingFor(Recent& a_r, Ev a_kind) {
            switch (a_kind) {
            case Ev::kEquip:     return &a_r.equip;
            case Ev::kLeaf:      return &a_r.leaf;
            case Ev::kCaster:    return &a_r.caster;
            case Ev::kCheckCast: return &a_r.check;
            case Ev::kStop:      return &a_r.stop;
            default:             return &a_r.anim;
            }
        }
        // Called from engine frames (equip worker, combat thread, CheckCast, anim sink): the map
        // insert may allocate, so nothing may escape (review F7) -- a failed record costs one
        // diagnosis fact, never an unwind into the engine.
        void Record(RE::FormID a_actor, Ev a_kind, Event a_ev) noexcept {
            try {
                a_ev.ms = apmf::clock::MonotonicMs();
                std::scoped_lock lk(g_recMx);
                if (g_recent.size() >= 4 * kMaxWatched && g_recent.find(a_actor) == g_recent.end()) return;   // bounded
                auto& r = g_recent[a_actor];
                r.lastMs = a_ev.ms;
                RingFor(r, a_kind)->push(a_ev);
            } catch (...) {
            }
        }

        // Pending InterruptCast events (anim thread -> game thread), under g_recMx.
        struct PendingInterrupt { RE::FormID actor; std::uint64_t ms; };
        std::vector<PendingInterrupt> g_pendingInterrupts;
        std::atomic<bool>             g_anyPending{ false };

        // Cast-relevant anim tags only (keeps the log to actual casting, not
        // footsteps/idles). Substring match against the fired event tag.
        constexpr std::array<std::string_view, 8> kCastTagNeedles{ {
            "Spell", "Cast", "Charge", "Release", "Aim", "MLh_", "MRh_", "Magic",
        } };

        bool IsCastRelevant(std::string_view tag) {
            for (auto needle : kCastTagNeedles)
                if (tag.find(needle) != std::string_view::npos) return true;
            return false;
        }

        class CastAnimSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent> {
        public:
            explicit CastAnimSink(RE::FormID a_fid) : fid(a_fid) {}

            RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
                                                  RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override {
                // PASSIVE: read the tag, maybe log, ALWAYS continue -- never consume,
                // never mutate. Off the game thread, so guard the dedup table.
                if (a_event) {
                    const char* tagC = a_event->tag.c_str();
                    const std::string_view tag = tagC ? std::string_view(tagC) : std::string_view{};
                    if (!tag.empty() && IsCastRelevant(tag)) {
                        // Caster-type census (core/CasterTypeCensus.h): counts BeginCast /
                        // SpellFire tags for an actor with an open claim window. Undeduped,
                        // relaxed pre-gate when no window is open, never mutates.
                        apmf::castertypecensus::NoteAnimEvent(fid, tag);
                        // Interrupt attribution / channel end (2026-10-06): a WATCHED actor's cast
                        // tags are recorded undeduped (copied, the pool string is not kept), and an
                        // InterruptCast queues an attribution line for the game thread.
                        if (Watching(fid)) {
                            Event ev{};
                            const std::size_t n = (std::min)(tag.size(), sizeof(ev.tag) - 1);
                            std::memcpy(ev.tag, tag.data(), n);
                            Record(fid, Ev::kAnim, ev);
                            if (tag == "InterruptCast") {
                                try {
                                    std::scoped_lock lk(g_recMx);
                                    if (g_pendingInterrupts.size() < 64)
                                        g_pendingInterrupts.push_back({ fid, apmf::clock::MonotonicMs() });
                                    g_anyPending.store(true, std::memory_order_release);
                                } catch (...) {
                                }
                            }
                        }
                        bool emit = false;
                        {
                            const auto now = apmf::clock::MonotonicMs();
                            std::scoped_lock lock(g_evMx);
                            auto& last = g_lastEvt[fid];   // {last tag ptr, last ms}
                            // Log a DISTINCT tag immediately (preserve the sequence);
                            // suppress an identical repeat within the dedup window.
                            if (last.first != tagC || now - last.second >= kEventDedupMs) {
                                last = { tagC, now };
                                emit = true;
                            }
                        }
                        if (emit) {
                            const char* payC = a_event->payload.c_str();
                            spdlog::info("[castobs] t={} 0x{} ANIM-EVENT '{}'{}{}",
                                         apmf::clock::MonotonicMs(), apmf::log::Hex(fid), tag,
                                         (payC && *payC) ? " payload=" : "", (payC && *payC) ? payC : "");
                        }
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }

            RE::FormID fid;
        };

        // Per-actor sinks, kept alive for the session (GAME THREAD ONLY access).
        std::unordered_map<RE::FormID, std::unique_ptr<CastAnimSink>> g_sinks;

        const char* CasterStateName(std::uint32_t s) {
            switch (s) {
            case 0:  return "None";
            case 1:  return "Unk1";
            case 2:  return "Charging";
            case 3:  return "Charged";
            case 4:  return "Casting";
            case 5:  return "Released";
            case 6:  return "Concluding";
            default: return "?";
            }
        }

        static_assert(offsetof(RE::Actor::ACTOR_RUNTIME_DATA, magicCasters) == 0xC0,
                      "magicCasters moved -- re-verify Character::GetMagicCaster's slot read (AE 0x6C20D0 "
                      "+0x1A8, SE 0x6301C0 +0x1A0) before trusting this read");

        // Read one hand's caster; log on a state/spell transition. Returns whether the
        // actor appears to be actively casting (so the poll can register the sink).
        bool ObserveHand(RE::Actor* a_actor, RE::MagicSystem::CastingSource a_src,
                         std::size_t a_srcIdx, const char* a_srcName) {
            // READ the slot, never `Actor::GetMagicCaster`: that call (Character vtable slot 0x5C,
            // AE 0x6C20D0 / SE 0x6301C0) ALLOCATES and installs a new ActorMagicCaster when the
            // slot is null, unlocked, off the actor's own update -- a write from a probe that must
            // be passive (review F2 on 58a4438). `magicCasters[]` is indexed by CastingSource, the
            // `[actor + src*8 + 0x1A8]` AE / `+0x1A0` SE read that function itself starts with.
            // Null = this actor has no such caster yet = nothing to observe.
            const auto idx = static_cast<std::size_t>(a_src);
            if (idx >= 4) return false;
            auto* mc = a_actor->GetActorRuntimeData().magicCasters[idx];
            if (!mc) return false;

            const std::uint32_t st    = static_cast<std::uint32_t>(mc->state.get());
            auto*               spell = mc->currentSpell;
            const RE::FormID    sfid  = spell ? spell->GetFormID() : 0;

            const std::uint64_t key = (static_cast<std::uint64_t>(a_actor->GetFormID()) << 3) | a_srcIdx;
            auto& prev = g_lastHand[key];
            const bool changed = (prev.state != st) || (prev.spell != sfid);
            if (changed) {
                prev.state = st;
                prev.spell = sfid;
                if (st != 0 || sfid != 0) {   // don't log the resting None/no-spell baseline
                    spdlog::info("[castobs] t={} 0x{} '{}' CASTER[{}] state={}({}) spell=0x{}",
                                 apmf::clock::MonotonicMs(), apmf::log::Hex(a_actor->GetFormID()),
                                 a_actor->GetName() ? a_actor->GetName() : "?", a_srcName, st,
                                 CasterStateName(st), apmf::log::Hex(sfid));
                }
            }
            return st != 0 || sfid != 0;
        }


        // =====================================================================
        // GAME THREAD: interrupt attribution + concentration channel end
        // (2026-10-06, see CastObserve.h items 3 and 4).
        // =====================================================================
        constexpr std::uint64_t kAttribGapMs     = 200;    // per actor
        constexpr std::uint64_t kChannelGapMs    = 300;    // per (actor, hand)
        constexpr std::uint32_t kLinesPerSecond  = 10;     // the two new line kinds together
        constexpr std::uint64_t kLeafWindowMs    = 1000;
        constexpr std::uint64_t kCasterWindowMs  = 1000;
        constexpr std::uint64_t kCheckWindowMs   = 500;
        constexpr std::uint64_t kEquipWindowMs   = 100;
        constexpr std::uint64_t kChannelWindowMs = 1000;

        std::uint64_t g_budgetSecond = 0;
        std::uint32_t g_budgetUsed   = 0;
        std::uint32_t g_budgetDropped = 0;
        // One shared budget for INTERRUPT-ATTRIB and CHANNEL-END. A dropped line is
        // counted and the count is printed with the next line (never hidden).
        bool LineBudget(std::uint64_t a_now, std::uint32_t& a_droppedBefore) {
            const auto sec = a_now / 1000;
            if (sec != g_budgetSecond) { g_budgetSecond = sec; g_budgetUsed = 0; }
            if (g_budgetUsed >= kLinesPerSecond) { ++g_budgetDropped; return false; }
            ++g_budgetUsed;
            a_droppedBefore = g_budgetDropped;
            g_budgetDropped = 0;
            return true;
        }

        std::unordered_map<std::uintptr_t, const char*> g_leafNames;   // built once, game thread only
        bool                                            g_leafNamesBuilt = false;
        void BuildLeafNames() {
            if (g_leafNamesBuilt) return;
            g_leafNamesBuilt = true;
            // Only where ch.7 resolves the same ids (an id miss is fatal in the fork; every
            // other build gets "?" names, and NoteLeafAct is never fed there anyway).
            if (!apmf::allowance::RuntimeSupported() || REL::Module::IsVR()) return;
            for (const auto& l : apmf::cbt::kLeaves) {
                REL::Relocation<std::uintptr_t> vt{ l.vtbl };
                g_leafNames[vt.address()] = l.name;
            }
            for (const auto& l : apmf::cbt::kSearchLeaves) {
                REL::Relocation<std::uintptr_t> vt{ l.vtbl };
                g_leafNames[vt.address()] = l.name;
            }
        }
        const char* LeafName(std::uintptr_t a_vt) {
            const auto it = g_leafNames.find(a_vt);
            if (it == g_leafNames.end()) return "?";
            std::string_view n = it->second;
            constexpr std::string_view kPrefix = "CombatBehavior";
            if (n.starts_with(kPrefix)) n.remove_prefix(kPrefix.size());
            return n.data();   // a suffix of a string literal: still NUL-terminated
        }

        const char* SourceTag(int a_src) {
            switch (a_src) {
            case 0:  return "L";
            case 1:  return "R";
            case 2:  return "voice";
            case 3:  return "instant";
            default: return "?";
            }
        }
        const char* ReasonName(std::uint32_t a_reason) {
            switch (a_reason) {
            case 0:   return "kOK";
            case 1:   return "kMagicka";
            case 2:   return "kPowerUsed";
            case 3:   return "kRangedUnderWater";
            case 4:   return "kMultipleCast";
            case 5:   return "kItemCharge";
            case 6:   return "kCastWhileShouting";
            case 7:   return "kShoutWhileCasting";
            case 8:   return "kShoutWhileRecovering";
            case 100: return "kCustomReasonNoStart";
            default:  return "?";
            }
        }

        // The events of one ring inside [now - window, now], oldest first, one text.
        // `a_source` filters CheckCast events to one caster source (-1 = all).
        template <std::size_t N>
        std::string FormatRing(const Ring<N>& a_ring, Ev a_kind, std::uint64_t a_now, std::uint64_t a_windowMs,
                               int a_source = -1) {
            std::vector<const Event*> evs;
            for (const auto& e : a_ring.e) {
                if (e.ms == 0 || e.ms > a_now || a_now - e.ms > a_windowMs) continue;
                if (a_kind == Ev::kCheckCast && a_source >= 0 && e.source != a_source) continue;
                evs.push_back(&e);
            }
            std::sort(evs.begin(), evs.end(), [](const Event* a, const Event* b) { return a->ms < b->ms; });
            std::string out;
            for (const auto* e : evs) {
                if (!out.empty()) out += ", ";
                const auto ago = a_now - e->ms;
                switch (a_kind) {
                case Ev::kEquip:
                    out += fmt::format("0x{} {} '{}' -{}ms", apmf::log::Hex(e->form), e->s1 ? e->s1 : "?",
                                       e->s2 ? e->s2 : "?", ago);
                    break;
                case Ev::kLeaf:
                    out += fmt::format("{} -{}ms", LeafName(e->vt), ago);
                    break;
                case Ev::kCaster:
                    out += fmt::format("{} 0x{} {} -{}ms", e->s1 ? e->s1 : "?", apmf::log::Hex(e->form),
                                       e->b1 ? "FIRED" : (e->b2 ? "CheckStartCast YES" : "CheckStartCast NO"), ago);
                    break;
                case Ev::kCheckCast:
                    out += fmt::format("{} 0x{} {} by {} -{}ms", SourceTag(e->source), apmf::log::Hex(e->form),
                                       ReasonName(e->u), e->b1 ? "Harbinger" : "engine", ago);
                    break;
                case Ev::kStop:
                    out += fmt::format("STOP ({}) -{}ms", e->s1 ? e->s1 : "?", ago);
                    break;
                default:
                    out += fmt::format("{} -{}ms", e->tag, ago);
                    break;
                }
            }
            return out.empty() ? std::string("none") : out;
        }

        bool SnapshotRecent(RE::FormID a_actor, Recent& a_out) {
            std::scoped_lock lk(g_recMx);
            const auto it = g_recent.find(a_actor);
            if (it == g_recent.end()) return false;
            a_out = it->second;
            return true;
        }

        std::string ClaimDesc(RE::FormID a_actor, apmf::CastHand a_hand) {
            apmf::CastSeatClaim c{};
            if (!apmf::ControlMap::Get().TryGetCastSeatClaimForHand(a_actor, a_hand, c)) return "-";
            if (c.flags & APMF_API::kCastFlag_DenyHandOnly)
                return (c.flags & APMF_API::kCastFlag_FloorSpellsOnly) ? "spells-only floor" : "deny-only floor";
            std::string d = "0x" + apmf::log::Hex(c.spell);
            if (c.proxy) d += " via 0x" + apmf::log::Hex(c.proxy);
            if (c.flags & APMF_API::kCastFlag_Concentration) d += " conc";
            d += " tgt=0x" + apmf::log::Hex(c.target);
            return d;
        }

        std::string CasterDesc(RE::Actor* a_actor, std::size_t a_idx) {
            // The slot read, never Actor::GetMagicCaster (it allocates on a null slot; see
            // ObserveHand below).
            auto* mc = a_actor->GetActorRuntimeData().magicCasters[a_idx];
            if (!mc) return "none";
            const auto st    = static_cast<std::uint32_t>(mc->state.get());
            auto*      spell = mc->currentSpell;
            return fmt::format("{}({}) 0x{}", st, CasterStateName(st), apmf::log::Hex(spell ? spell->GetFormID() : 0));
        }

        std::unordered_map<RE::FormID, std::uint64_t> g_lastAttribMs;   // game thread only

        void DrainInterrupts(std::uint64_t a_now) {
            if (!g_anyPending.load(std::memory_order_acquire)) return;
            std::vector<PendingInterrupt> work;
            {
                std::scoped_lock lk(g_recMx);
                work.swap(g_pendingInterrupts);
                g_anyPending.store(false, std::memory_order_relaxed);
            }
            for (const auto& p : work) {
                auto& last = g_lastAttribMs[p.actor];
                if (last != 0 && p.ms - last < kAttribGapMs) continue;   // one line per burst
                last = p.ms;
                std::uint32_t dropped = 0;
                if (!LineBudget(a_now, dropped)) continue;
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(p.actor);
                if (!actor) continue;
                Recent snap{};
                SnapshotRecent(p.actor, snap);
                const auto& rd = actor->GetActorRuntimeData();
                spdlog::info("[castobs] INTERRUPT-ATTRIB t={} 0x{} '{}' (+{} ms after the InterruptCast tag) | claims "
                             "L={} R={} | casters L={} R={} I={} | combat controller {} | action (leaves, last {} ms): "
                             "{} | magic caster (last {} ms): {} | CheckCast NO (last {} ms): {} | equips (last {} ms): "
                             "{}{}",
                             p.ms, apmf::log::Hex(p.actor), actor->GetName() ? actor->GetName() : "?",
                             a_now >= p.ms ? a_now - p.ms : 0,
                             ClaimDesc(p.actor, apmf::CastHand::kLeft), ClaimDesc(p.actor, apmf::CastHand::kRight),
                             CasterDesc(actor, 0), CasterDesc(actor, 1), CasterDesc(actor, 3),
                             rd.combatController ? "yes" : "none",
                             kLeafWindowMs, FormatRing(snap.leaf, Ev::kLeaf, p.ms, kLeafWindowMs),
                             kCasterWindowMs, FormatRing(snap.caster, Ev::kCaster, p.ms, kCasterWindowMs),
                             kCheckWindowMs, FormatRing(snap.check, Ev::kCheckCast, p.ms, kCheckWindowMs),
                             kEquipWindowMs, FormatRing(snap.equip, Ev::kEquip, p.ms, kEquipWindowMs),
                             dropped ? fmt::format(" ({} castobs line(s) dropped by the rate cap before this one)", dropped)
                                     : std::string());
            }
        }

        // ---- concentration channel watches (game thread only) ----
        struct ChannelWatch {
            RE::FormID    actor  = 0;
            int           hand   = 0;   // 0 L, 1 R (the magicCasters index)
            RE::FormID    driven = 0;
            RE::FormID    spell  = 0;
            RE::FormID    target = 0;
            std::uint32_t lastState = 0;
            bool          sawActive = false;
            bool          claimLive = true;
            std::uint64_t firstActiveMs = 0;
            std::uint64_t lastLineMs = 0;
            std::vector<std::pair<std::uint64_t, std::uint32_t>> hist;   // (ms, state), bounded
        };
        constexpr std::size_t     kMaxChannels = 16;
        constexpr std::size_t     kMaxHist     = 24;
        std::vector<ChannelWatch> g_channels;

        void LogChannelEnd(RE::Actor* a_actor, ChannelWatch& a_w, std::uint64_t a_now) {
            if (a_w.lastLineMs != 0 && a_now - a_w.lastLineMs < kChannelGapMs) return;
            std::uint32_t dropped = 0;
            if (!LineBudget(a_now, dropped)) return;
            a_w.lastLineMs = a_now;
            Recent snap{};
            SnapshotRecent(a_w.actor, snap);

            std::string states;
            const std::uint64_t base = a_w.hist.empty() ? a_now : a_w.hist.front().first;
            for (const auto& [ms, st] : a_w.hist)
                states += fmt::format("{}{}({})@+{}", states.empty() ? "" : " ", st, CasterStateName(st), ms - base);

            RE::FormID    cs = 0, cp = 0;
            std::uint32_t cf = 0;
            const bool claimHere = apmf::ControlMap::Get().TryGetCastClaimForHand(
                a_w.actor, a_w.hand == 0 ? apmf::CastHand::kLeft : apmf::CastHand::kRight, cs, cp, &cf);
            const bool sameClaim = claimHere && (cs == a_w.spell || (cp != 0 && cp == a_w.driven));

            float pct = -1.0f;
            if (auto* avo = a_actor->AsActorValueOwner()) {
                const float perm = avo->GetPermanentActorValue(RE::ActorValue::kMagicka);
                if (perm > 0.0f) pct = 100.0f * avo->GetActorValue(RE::ActorValue::kMagicka) / perm;
            }
            std::string los = "n/a (no target)";
            if (a_w.target != 0 && a_w.target != a_w.actor) {
                const auto r = apmf::sightline::Read(a_w.actor, a_w.target, false);   // stored reading, never asks
                los = r.ageMs == 0xFFFFFFFFu
                          ? std::string("never measured")
                          : fmt::format("{} age {} ms, {} occluded in a row", apmf::sightline::VerdictName(r.verdict),
                                        r.ageMs, r.occRun);
            }
            spdlog::info("[castobs] CHANNEL-END t={} 0x{} '{}' hand={} driven=0x{} spell=0x{} target=0x{} -- the "
                         "claimed hand's caster returned to 0 after {} ms of channel | states {} | seat 0x07 (last {} "
                         "ms): {} | CheckCast NO on this hand (last {} ms): {} | cast anim tags (last {} ms): {} | action "
                         "(leaves, last {} ms): {} | claim {} | magicka {} | own line of sight {} | engine LoS re-check: "
                         "not seated (unobserved){}",
                         a_now, apmf::log::Hex(a_w.actor), a_actor->GetName() ? a_actor->GetName() : "?",
                         a_w.hand == 0 ? "L" : "R", apmf::log::Hex(a_w.driven), apmf::log::Hex(a_w.spell),
                         apmf::log::Hex(a_w.target), a_w.firstActiveMs ? a_now - a_w.firstActiveMs : 0, states,
                         kChannelWindowMs, FormatRing(snap.stop, Ev::kStop, a_now, kChannelWindowMs),
                         kChannelWindowMs, FormatRing(snap.check, Ev::kCheckCast, a_now, kChannelWindowMs, a_w.hand),
                         kChannelWindowMs, FormatRing(snap.anim, Ev::kAnim, a_now, kChannelWindowMs),
                         // The same behaviour-tree leaf ring INTERRUPT-ATTRIB prints (the AI's
                         // current ACTION around the end of the channel), same window.
                         kLeafWindowMs, FormatRing(snap.leaf, Ev::kLeaf, a_now, kLeafWindowMs),
                         sameClaim ? "still stands" : claimHere ? "replaced on this hand" : "gone (released or lapsed)",
                         pct < 0.0f ? std::string("?") : fmt::format("{:.0f}%", pct), los,
                         dropped ? fmt::format(" ({} castobs line(s) dropped by the rate cap before this one)", dropped)
                                 : std::string());
        }

        // Every frame: read each watched concentration hand's caster.
        void TrackChannels(std::uint64_t a_now) {
            if (g_channels.empty()) return;
            for (auto it = g_channels.begin(); it != g_channels.end();) {
                auto& w     = *it;
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(w.actor);
                if (!actor) { it = g_channels.erase(it); continue; }
                auto*               mc    = actor->GetActorRuntimeData().magicCasters[static_cast<std::size_t>(w.hand)];
                const std::uint32_t st    = mc ? static_cast<std::uint32_t>(mc->state.get()) : 0;
                auto*               sp    = mc ? mc->currentSpell : nullptr;
                const RE::FormID    spell = sp ? sp->GetFormID() : 0;
                if (st != w.lastState) {
                    if (w.hist.size() >= kMaxHist) w.hist.erase(w.hist.begin());
                    w.hist.emplace_back(a_now, st);
                    w.lastState = st;
                }
                if (st >= 4 && spell != 0 && (spell == w.driven || spell == w.spell) && !w.sawActive) {
                    w.sawActive     = true;
                    w.firstActiveMs = a_now;
                }
                if (st == 0) {
                    if (w.sawActive) LogChannelEnd(actor, w, a_now);
                    w.sawActive     = false;
                    w.firstActiveMs = 0;
                    w.hist.clear();
                    w.hist.emplace_back(a_now, 0u);
                    if (!w.claimLive) { it = g_channels.erase(it); continue; }
                }
                ++it;
            }
        }

        // 100 ms: the watched set (any thread reads it) and the concentration watches.
        void RefreshWatch(std::uint64_t a_now) {
            std::vector<RE::Actor*> actors;
            if (apmf::ControlMap::Get().ControlledCount() != 0)
                actors = apmf::ControlMap::Get().ClaimedActors(APMF_API::kIntent_Cast);
            std::uint32_t n = 0;
            for (auto* a : actors) {
                if (!a || n >= kMaxWatched) continue;
                g_watch[n++].store(a->GetFormID(), std::memory_order_relaxed);
            }
            for (std::uint32_t i = n; i < kMaxWatched; ++i) g_watch[i].store(0, std::memory_order_relaxed);
            g_watchN.store(n, std::memory_order_release);
            if (n) BuildLeafNames();

            for (auto& w : g_channels) w.claimLive = false;
            for (auto* a : actors) {
                if (!a) continue;
                const RE::FormID fid = a->GetFormID();
                for (int hand = 0; hand < 2; ++hand) {
                    apmf::CastSeatClaim c{};
                    if (!apmf::ControlMap::Get().TryGetCastSeatClaimForHand(
                            fid, hand == 0 ? apmf::CastHand::kLeft : apmf::CastHand::kRight, c))
                        continue;
                    if (c.flags & APMF_API::kCastFlag_DenyHandOnly) continue;
                    const RE::FormID driven = c.proxy ? c.proxy : c.spell;
                    if (!driven) continue;
                    bool conc = (c.flags & APMF_API::kCastFlag_Concentration) != 0;
                    if (!conc) {
                        auto* mi = RE::TESForm::LookupByID<RE::MagicItem>(driven);
                        conc     = mi && mi->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
                    }
                    if (!conc) continue;
                    auto wit = std::find_if(g_channels.begin(), g_channels.end(),
                                            [&](const ChannelWatch& w) { return w.actor == fid && w.hand == hand; });
                    if (wit == g_channels.end()) {
                        if (g_channels.size() >= kMaxChannels) continue;
                        ChannelWatch w{};
                        w.actor = fid;
                        w.hand  = hand;
                        w.hist.emplace_back(a_now, 0u);
                        g_channels.push_back(std::move(w));
                        wit = std::prev(g_channels.end());
                    }
                    wit->driven    = driven;
                    wit->spell     = c.spell;
                    wit->target    = c.target;
                    wit->claimLive = true;
                }
            }
            // A watch whose claim is gone and whose hand is idle ends here (one whose hand
            // is still channelling is kept until it returns to 0: TrackChannels).
            g_channels.erase(std::remove_if(g_channels.begin(), g_channels.end(),
                                            [](const ChannelWatch& w) { return !w.claimLive && !w.sawActive; }),
                             g_channels.end());

            // Bound the recorder map to the watched set (+ a short tail for a late drain).
            std::scoped_lock lk(g_recMx);
            for (auto it = g_recent.begin(); it != g_recent.end();) {
                if (!Watching(it->first) && a_now - it->second.lastMs > 5000) it = g_recent.erase(it);
                else ++it;
            }
            if (g_lastAttribMs.size() > 256) g_lastAttribMs.clear();
        }
    }

    void Poll() {
        // GAME THREAD (Arbiter::OncePerFrame == the PlayerCharacter/Drain seat, main
        // thread). The interrupt drain and the channel tracker run every frame (one
        // relaxed load / an empty-vector test when idle); the state poll self-throttles.
        const auto now = apmf::clock::MonotonicMs();
        DrainInterrupts(now);
        TrackChannels(now);
        if (now - g_lastPollMs < kPollIntervalMs) return;
        g_lastPollMs = now;
        RefreshWatch(now);

        auto* pl = RE::ProcessLists::GetSingleton();
        if (!pl) return;

        // Main-thread read of highActorHandles (resize is main-thread too).
        for (auto& handle : pl->highActorHandles) {
            auto a = handle.get();
            if (!a) continue;
            auto* actor = a.get();
            if (!actor || actor->IsPlayerRef()) continue;

            bool casting = false;
            casting |= ObserveHand(actor, RE::MagicSystem::CastingSource::kLeftHand,  0, "L");
            casting |= ObserveHand(actor, RE::MagicSystem::CastingSource::kRightHand, 1, "R");
            casting |= ObserveHand(actor, RE::MagicSystem::CastingSource::kInstant,   2, "I");

            // Register the passive anim sink once, when first seen casting, so the
            // NEXT events (and every subsequent cast) capture the tag sequence.
            if (casting) {
                const RE::FormID fid = actor->GetFormID();
                if (g_sinks.find(fid) == g_sinks.end()) {
                    auto sink = std::make_unique<CastAnimSink>(fid);
                    actor->AddAnimationGraphEventSink(sink.get());
                    g_sinks.emplace(fid, std::move(sink));
                    spdlog::info("[castobs] t={} 0x{} '{}' -- anim-event sink registered (observe-only).",
                                 now, apmf::log::Hex(fid), actor->GetName() ? actor->GetName() : "?");
                }
            }
        }
    }

    bool AnyWatched() { return g_watchN.load(std::memory_order_relaxed) != 0; }

    bool Watching(RE::FormID a_actor) {
        if (a_actor == 0 || g_watchN.load(std::memory_order_acquire) == 0) return false;
        for (const auto& w : g_watch)
            if (w.load(std::memory_order_relaxed) == a_actor) return true;
        return false;
    }

    void NoteEquip(RE::FormID a_actor, RE::FormID a_item, const char* a_path, const char* a_verdict) {
        if (!Watching(a_actor)) return;
        Event e{};
        e.form = a_item;
        e.s1   = a_path;
        e.s2   = a_verdict;
        Record(a_actor, Ev::kEquip, e);
    }

    void NoteLeafAct(RE::FormID a_actor, std::uintptr_t a_leafVtable) {
        if (!Watching(a_actor)) return;
        Event e{};
        e.vt = a_leafVtable;
        Record(a_actor, Ev::kLeaf, e);
    }

    void NoteCaster(RE::FormID a_actor, const char* a_type, RE::FormID a_item, bool a_fired, bool a_answer) {
        if (!Watching(a_actor)) return;
        Event e{};
        e.form = a_item;
        e.s1   = a_type;
        e.b1   = a_fired;
        e.b2   = a_answer;
        Record(a_actor, Ev::kCaster, e);
    }

    void NoteCheckCast(RE::FormID a_actor, int a_source, RE::FormID a_spell, std::uint32_t a_reason, bool a_byApmf) {
        if (!Watching(a_actor)) return;
        Event e{};
        e.form   = a_spell;
        e.source = static_cast<std::int8_t>(a_source);
        e.u      = a_reason;
        e.b1     = a_byApmf;
        Record(a_actor, Ev::kCheckCast, e);
    }

    void NoteStopCast(RE::FormID a_actor, const char* a_why) {
        if (!Watching(a_actor)) return;
        Event e{};
        e.s1 = a_why;
        Record(a_actor, Ev::kStop, e);
    }

}
