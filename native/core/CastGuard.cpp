#include "PCH.h"
#include "core/Log.h"
#include "core/Clock.h"
#include "core/CastGuard.h"
#include "core/ControlMap.h"
#include "core/Registry.h"
#include "core/Sightline.h"
#include "core/Sources.h"
#include "core/CastGate.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

// ============================================================================
// ch.25 CAST GUARD -- the measurement, the seat decisions, and the two ControlMap readers.
// See core/CastGuard.h for the contract and the evidence.
// ============================================================================

namespace apmf {

    // ---- ControlMap readers for ch.25 (declared in core/ControlMap.h) -------------------
    // Defined here, not in ControlMap.cpp, which is at its 2500-line cap. Same discipline as
    // ControlMap::OwnLosPinPairs: relaxed pre-gate, one acquire-load of a frozen snapshot, copy
    // out, mutate nothing. Winner = highest basis, tie -> earliest (guard claims carry no cast
    // flags, so BetterClaim's deny-only rule could not change it); a lapsed claim is skipped as
    // a candidate so it can neither answer nor mask a live lower-basis one before the sweep.
    namespace {
        // A template so it can rank ControlMap's PRIVATE Claim type without naming it here.
        template <class C>
        const C* BestLiveGuard(const std::vector<C>& a_claims, std::uint64_t a_now) {
            const C* best = nullptr;
            for (const auto& c : a_claims) {
                if (c.expiresMs != 0 && a_now >= c.expiresMs) continue;
                if (!best || c.basis > best->basis) best = &c;
            }
            return best;
        }
    }

    bool ControlMap::TryGetCastGuard(RE::FormID actor, std::uint32_t& outFlags, Handle& outHandle) const {
        outFlags  = 0;
        outHandle = APMF_API::kInvalidHandle;
        if (m_anyControlled.load(std::memory_order_relaxed) == 0) return false;
        std::shared_ptr<const MapType> snap = m_published.load(std::memory_order_acquire);
        auto it = snap->find(actor);
        if (it == snap->end()) return false;
        auto* channel = Registry::Get().ChannelForIntent(APMF_API::kIntent_CastGuard);
        if (!channel) return false;
        for (const auto& cs : it->second.channels) {
            if (cs.channel != channel) continue;
            const Claim* best = BestLiveGuard(cs.claims, apmf::clock::MonotonicMs());
            if (!best) return false;
            outFlags  = static_cast<std::uint32_t>(best->param.ival) & APMF_API::kCastGuard_All;
            outHandle = best->handle;
            return true;
        }
        return false;
    }

    void ControlMap::CastGuardActors(std::vector<std::pair<RE::FormID, std::uint32_t>>& out) const {
        if (m_anyControlled.load(std::memory_order_relaxed) == 0) return;
        std::shared_ptr<const MapType> snap = m_published.load(std::memory_order_acquire);
        auto* channel = Registry::Get().ChannelForIntent(APMF_API::kIntent_CastGuard);
        if (!channel) return;
        const auto now = apmf::clock::MonotonicMs();
        for (const auto& [fid, npc] : *snap) {
            if (!npc.handle.get()) continue;   // unloaded; the Drain sweep reclaims it
            for (const auto& cs : npc.channels) {
                if (cs.channel != channel) continue;
                if (const Claim* best = BestLiveGuard(cs.claims, now))
                    out.emplace_back(fid, static_cast<std::uint32_t>(best->param.ival) & APMF_API::kCastGuard_All);
            }
        }
    }

}

namespace apmf::castguard {

    namespace {

        using apmf::log::Hex;

        std::atomic<bool>        g_armed{ false };
        // Pre-gate for the seats: true while any actor holds a live guard claim. Written by Pump right
        // after ControlMap::Drain on the same main-thread seat, so a new claim is seen the frame it
        // publishes; a release may be honoured one frame late (the reader re-checks the claim anyway).
        std::atomic<bool>        g_anyGuard{ false };
        std::atomic<const char*> g_notArmedReason{ "before kDataLoaded (the measurement arms there)" };

        // ---- the published measurement: one SEQLOCK slot per guarded caster ---------------
        // Writer: the main thread only (Pump / ResetAll). Readers: any thread, lock-free (the
        // cast seats). Payload: {key = caster, aim}, {line friend, blast friend}, {line flag,
        // blast distance bits}, measuredMs. kSlots is far above any realistic number of guarded
        // casters; a full table is logged and the caster then reads UNKNOWN (never clear).
        constexpr std::size_t kSlots = 128;   // power of two
        constexpr std::size_t kProbe = 16;
        static_assert((kSlots & (kSlots - 1)) == 0);

        struct Slot {
            std::atomic<std::uint32_t> seq{ 0 };
            std::atomic<std::uint64_t> key{ 0 };       // (caster << 32) | aim; caster 0 = empty
            std::atomic<std::uint64_t> friends{ 0 };   // (lineFriend << 32) | blastFriend
            std::atomic<std::uint64_t> word{ 0 };      // (lineBlocked << 32) | bits(blastDist)
            std::atomic<std::uint64_t> measuredMs{ 0 };
        };
        Slot g_slots[kSlots];

        struct Lof {
            bool          found       = false;   // a consistent slot for this caster was read
            RE::FormID    aim         = 0;       // 0 = no aim (nothing to fire along: clear)
            RE::FormID    lineFriend  = 0;
            RE::FormID    blastFriend = 0;
            bool          lineBlocked = false;
            float         blastDist   = std::numeric_limits<float>::infinity();
            std::uint64_t measuredMs  = 0;
        };

        constexpr RE::FormID KeyCaster(std::uint64_t k) { return static_cast<RE::FormID>(k >> 32); }

        std::size_t Home(RE::FormID a_caster) {
            std::uint64_t z = a_caster + 0x9E3779B97F4A7C15ull;   // splitmix64 finalizer
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z ^= z >> 31;
            return static_cast<std::size_t>(z) & (kSlots - 1);
        }

        void Publish(Slot& s, const Lof& a_l, RE::FormID a_caster) {
            const std::uint32_t q = s.seq.load(std::memory_order_relaxed);
            s.seq.store(q + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            s.key.store((static_cast<std::uint64_t>(a_caster) << 32) | a_l.aim, std::memory_order_relaxed);
            s.friends.store((static_cast<std::uint64_t>(a_l.lineFriend) << 32) | a_l.blastFriend,
                            std::memory_order_relaxed);
            s.word.store((static_cast<std::uint64_t>(a_l.lineBlocked ? 1u : 0u) << 32) |
                             std::bit_cast<std::uint32_t>(a_l.blastDist),
                         std::memory_order_relaxed);
            s.measuredMs.store(a_l.measuredMs, std::memory_order_relaxed);
            s.seq.store(q + 2, std::memory_order_release);
        }

        void Empty(Slot& s) {
            const std::uint32_t q = s.seq.load(std::memory_order_relaxed);
            s.seq.store(q + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            s.key.store(0, std::memory_order_relaxed);
            s.measuredMs.store(0, std::memory_order_relaxed);
            s.seq.store(q + 2, std::memory_order_release);
        }

        // ANY thread. Never blocks; a persistently torn slot reads as not found (unknown).
        Lof Read(RE::FormID a_caster) {
            Lof l;
            const std::size_t h = Home(a_caster);
            for (std::size_t i = 0; i < kProbe; ++i) {
                const Slot& s = g_slots[(h + i) & (kSlots - 1)];
                for (int attempt = 0; attempt < 3; ++attempt) {
                    const std::uint32_t s1 = s.seq.load(std::memory_order_acquire);
                    if (s1 & 1u) continue;
                    const std::uint64_t k = s.key.load(std::memory_order_relaxed);
                    const std::uint64_t f = s.friends.load(std::memory_order_relaxed);
                    const std::uint64_t w = s.word.load(std::memory_order_relaxed);
                    const std::uint64_t m = s.measuredMs.load(std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_acquire);
                    if (s.seq.load(std::memory_order_relaxed) != s1) continue;
                    if (KeyCaster(k) != a_caster) break;   // a different caster: next probe slot
                    l.found       = true;
                    l.aim         = static_cast<RE::FormID>(k & 0xFFFFFFFFu);
                    l.lineFriend  = static_cast<RE::FormID>(f >> 32);
                    l.blastFriend = static_cast<RE::FormID>(f & 0xFFFFFFFFu);
                    l.lineBlocked = (w >> 32) != 0;
                    l.blastDist   = std::bit_cast<float>(static_cast<std::uint32_t>(w & 0xFFFFFFFFu));
                    l.measuredMs  = m;
                    return l;
                }
            }
            return l;
        }

        // MAIN thread: the slot for a_caster, taking an empty one in its probe window.
        Slot* SlotFor(RE::FormID a_caster) {
            const std::size_t h = Home(a_caster);
            Slot*             empty = nullptr;
            for (std::size_t i = 0; i < kProbe; ++i) {
                Slot&      s = g_slots[(h + i) & (kSlots - 1)];
                const auto k = s.key.load(std::memory_order_relaxed);
                if (KeyCaster(k) == a_caster) return &s;
                if (k == 0 && !empty) empty = &s;
            }
            return empty;
        }

        // ---- counters (relaxed; heartbeat on the main thread) ----
        std::atomic<std::uint64_t> g_measures{ 0 }, g_noAim{ 0 }, g_lineHits{ 0 }, g_blastNear{ 0 },
            g_lofRefused{ 0 }, g_chanRefused{ 0 }, g_startHeld{ 0 }, g_startUnknown{ 0 }, g_stood{ 0 },
            g_tableFull{ 0 };
        std::uint64_t g_lastHeartbeatMs = 0;   // main thread
        constexpr std::uint64_t kHeartbeatMs = 30000;

        // Decision log throttle: per (actor, subject, reason), 1500 ms -- the seats' cadence.
        constexpr std::uint64_t kLogThrottleMs = 1500;
        std::mutex                                       g_logMx;
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastLog;
        bool LogDue(RE::FormID a_actor, RE::FormID a_subject, std::uint32_t a_reason) {
            const auto now = apmf::clock::MonotonicMs();
            const auto key = (static_cast<std::uint64_t>(a_actor) << 32) ^ (static_cast<std::uint64_t>(a_subject)) ^
                             (static_cast<std::uint64_t>(a_reason) << 60);
            std::scoped_lock lk(g_logMx);
            auto& last = g_lastLog[key];
            if (now - last < kLogThrottleMs) return false;
            last = now;
            return true;
        }

        // ---- spell facts (immutable form data; any thread) ----
        // OFFENSIVE: any effect whose base effect is hostile, and not Self delivered (a self-
        // delivered spell is never fired along a line). Spells, scrolls and staff enchantments
        // alike -- whatever a hand caster deliberates.
        bool Offensive(RE::MagicItem* a_mi) {
            if (!a_mi || a_mi->GetDelivery() == RE::MagicSystem::Delivery::kSelf) return false;
            for (auto* eff : a_mi->effects)
                if (eff && eff->baseEffect && eff->baseEffect->IsHostile()) return true;
            return false;
        }
        // The largest effect area IN GAME UNITS (MagicItem::GetLargestArea is an id call with no
        // verified row, so the effect list is read directly). EffectItem::area is in FEET: Skyrim.esm
        // Fireball has area 15 and a 320u explosion radius, Fire Storm area 100 and 2100u -- 64/3 u per
        // foot (review SEV-2 of d2d591d; MFO's raw-number rule under-sized every blast ~21x).
        float LargestArea(RE::MagicItem* a_mi) {
            std::uint32_t a = 0;
            for (auto* eff : a_mi->effects)
                if (eff) a = std::max(a, eff->effectItem.area);
            return static_cast<float>(a) * kUnitsPerFoot;
        }
        // The channel bound applies to SPELLS (MFO's bound: a SpellItem), concentration cast.
        bool ConcentrationSpell(RE::MagicItem* a_mi) {
            return a_mi && a_mi->Is(RE::FormType::Spell) &&
                   a_mi->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
        }

        // ---- geometry (main thread) ----
        // Distance from p to segment [a,b], only when p projects BETWEEN the endpoints (an
        // actor behind the caster or past the aim is not on the line); +inf otherwise.
        float SegDist(const RE::NiPoint3& p, const RE::NiPoint3& a, const RE::NiPoint3& b) {
            const RE::NiPoint3 ab = b - a, ap = p - a;
            const float        len2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
            if (len2 <= 1.0f) return std::numeric_limits<float>::infinity();
            const float t = (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2;
            if (t < 0.0f || t > 1.0f) return std::numeric_limits<float>::infinity();
            const RE::NiPoint3 proj{ a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t };
            return (p - proj).Length();
        }

        // A friendly of the caster: not hostile to it (the engine's own test, asked from the
        // candidate as SpaceQuery asks it), and for a player-teammate caster also the player and
        // every teammate whatever that test says.
        bool Friendly(RE::Actor* a_caster, RE::Actor* a_other, RE::PlayerCharacter* a_player) {
            if (a_caster->IsPlayerTeammate() && (a_other == a_player || a_other->IsPlayerTeammate())) return true;
            return !a_other->IsHostileToActor(a_caster);
        }

        // Friendlies farther than this from the aim are not recorded for the blast test: 4096u covers
        // an area up to ~188 ft (Fire Storm's 100 ft = 2133u fits; a larger area is compared against
        // the nearest friendly inside 4096u). The hostility test runs only for actors this close.
        constexpr float kBlastConsider = 4096.0f;

        // MAIN thread: measure one caster and publish.
        void Measure(RE::FormID a_fid, std::uint64_t a_now) {
            Lof l;
            l.measuredMs = a_now;
            auto* caster = RE::TESForm::LookupByID<RE::Actor>(a_fid);
            if (!caster || !caster->Is3DLoaded() || !caster->GetParentCell()) return;   // stays unknown

            // The aim: the target of the live driving cast claim when that claim's spell is itself
            // OFFENSIVE (the seats aim it there), else the combat target the AI's own casts aim at. A
            // heal / buff claim at an ally is not a line anything offensive is fired along.
            RE::NiPointer<RE::Actor> aimPtr;
            apmf::CastSeatClaim      cl{};
            if (ControlMap::Get().TryGetCastSeatClaim(a_fid, cl) &&
                (cl.flags & APMF_API::kCastFlag_DenyHandOnly) == 0 && cl.target != 0 && cl.target != a_fid &&
                Offensive(RE::TESForm::LookupByID<RE::MagicItem>(cl.proxy ? cl.proxy : cl.spell)))
                aimPtr = cl.targetHandle.get();
            if (!aimPtr) aimPtr = caster->GetActorRuntimeData().currentCombatTarget.get();
            RE::Actor* aim = aimPtr.get();

            Slot* slot = SlotFor(a_fid);
            if (!slot) {
                g_tableFull.fetch_add(1, std::memory_order_relaxed);
                return;   // reads unknown: 0x06 holds, 0x0A does not refuse
            }
            g_measures.fetch_add(1, std::memory_order_relaxed);
            if (!aim || aim == caster) {
                g_noAim.fetch_add(1, std::memory_order_relaxed);
                Publish(*slot, l, a_fid);   // aim 0: no line to fire along
                return;
            }
            l.aim = aim->GetFormID();
            const RE::NiPoint3 cpos = caster->GetPosition();
            const RE::NiPoint3 tpos = aim->GetPosition();
            auto* const        ws   = caster->GetWorldspace();
            auto* const        cell = caster->GetParentCell();
            auto* const        player = RE::PlayerCharacter::GetSingleton();
            float              lineBest = std::numeric_limits<float>::infinity();

            const auto consider = [&](RE::Actor* a) {
                if (!a || a == caster || a == aim || a->IsDead() || a->IsDisabled()) return;
                if (ws ? a->GetWorldspace() != ws : a->GetParentCell() != cell) return;
                const RE::NiPoint3 p     = a->GetPosition();
                const float        seg   = SegDist(p, cpos, tpos);
                const float        blast = p.GetDistance(tpos);
                if (seg > kLinePad && blast > kBlastConsider) return;
                if (!Friendly(caster, a, player)) return;
                if (seg <= kLinePad) {
                    // Exposed to the caster unless Harbinger's own ray reads the friendly OCCLUDED
                    // (a wall between them). Asking keeps the pair measured; unknown counts.
                    const auto r = apmf::sightline::Read(a_fid, a->GetFormID(), true);
                    if (apmf::sightline::FreshVerdict(r) != APMF_API::kLos_Occluded && seg < lineBest) {
                        lineBest      = seg;
                        l.lineBlocked = true;
                        l.lineFriend  = a->GetFormID();
                    }
                }
                if (blast < l.blastDist) {
                    l.blastDist   = blast;
                    l.blastFriend = a->GetFormID();
                }
            };
            if (auto* pl = RE::ProcessLists::GetSingleton()) {
                pl->ForEachHighActor([&](RE::Actor* a) {
                    if (a != player) consider(a);   // the player is considered once, below
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            if (player) consider(player);
            if (l.lineBlocked) g_lineHits.fetch_add(1, std::memory_order_relaxed);
            if (l.blastFriend) g_blastNear.fetch_add(1, std::memory_order_relaxed);
            Publish(*slot, l, a_fid);
        }

        // Round robin over the guarded casters (main thread).
        std::vector<std::pair<RE::FormID, std::uint32_t>> g_guards;   // reused buffer
        std::size_t                                      g_rr = 0;
        std::vector<RE::FormID>                          g_live;      // reused buffer

        // The decision, attributed. True = the refusal stands (the guard's client outranks the
        // engine's own AI on the Cast facet).
        bool Decide(APMF_API::Handle a_h, RE::FormID a_actor, RE::Actor* a_who, RE::MagicItem* a_spell,
                    const char* a_seat, const char* a_what, std::uint32_t a_reason, const std::string& a_detail) {
            using apmf::sources::Facet;
            apmf::sources::SourceId client = apmf::sources::kNoSource;
            const int  claimPrio  = apmf::sources::ClaimPriority(a_h, Facet::kCast, &client);
            const int  enginePrio = apmf::sources::Priority(apmf::sources::kEngineSource, Facet::kCast);
            const bool stands     = claimPrio > enginePrio;
            apmf::sources::NoteHit(apmf::sources::kEngineSource, Facet::kCast);
            if (stands) apmf::sources::NoteOverruled(apmf::sources::kEngineSource, Facet::kCast);
            else {
                apmf::sources::NoteStood(apmf::sources::kEngineSource, Facet::kCast);
                g_stood.fetch_add(1, std::memory_order_relaxed);
            }
            const RE::FormID sf = a_spell ? a_spell->GetFormID() : 0;
            if (LogDue(a_actor, sf, a_reason)) {
                const auto who = a_who && a_who->GetName() ? a_who->GetName() : "?";
                spdlog::info("[ch.25] 0x{} '{}' {} {} spell 0x{} '{}' -- {}: {}. Guard h={} ({} priority {}) {} Engine "
                             "(priority {}) on the Cast facet{}.",
                             Hex(a_actor), who, a_seat, stands ? "REFUSED" : "would refuse, ALLOWED:", Hex(sf),
                             a_spell && a_spell->GetName() ? a_spell->GetName() : "?", a_what, a_detail, a_h,
                             client != apmf::sources::kNoSource ? std::string(apmf::sources::Name(client))
                                                                : std::string("an unmapped client"),
                             claimPrio, stands ? "outranks" : "does not outrank", enginePrio,
                             stands ? "" : " -- the engine's own AI stands (APMF_Sources.ini)");
            }
            return stands;
        }

        std::string LofDetail(const Lof& l, float a_area, bool a_line, std::uint64_t a_now) {
            if (a_line)
                return fmt::format("friendly 0x{} is within {:.0f}u of the line to 0x{} (measured {} ms ago)",
                                   Hex(l.lineFriend), kLinePad, Hex(l.aim), a_now - l.measuredMs);
            return fmt::format("friendly 0x{} is {:.0f}u from the aim 0x{}, inside the spell's area {:.0f} + {:.0f}u "
                               "(measured {} ms ago)",
                               Hex(l.blastFriend), l.blastDist, Hex(l.aim), a_area, kBlastPad, a_now - l.measuredMs);
        }

        enum Reason : std::uint32_t { kReasonLine = 1, kReasonBlast = 2, kReasonUnknown = 3, kReasonChannel = 4 };

    }

    void Install() {
        if (g_armed.load(std::memory_order_relaxed)) return;
        if (REL::Module::IsVR()) {
            g_notArmedReason.store("VR runtime (the hostility call is verified on 1.6.1170, 1.5.97 and 1.7.104 only)");
        } else if (!allowance::RuntimeSupported()) {
            g_notArmedReason.store("this runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104");
        } else if (!allowance::SeatVerified(
                       REL::Relocation<std::uintptr_t>{ RELOCATION_ID(36537, 37537) }.address(),
                       "CastGuard.IsHostileToActor (the SpaceQuery.IsHostileToActor row)")) {
            g_notArmedReason.store("the mit-3.7 self-check refused Actor::IsHostileToActor");
        } else {
            g_armed.store(true, std::memory_order_release);
            spdlog::info("[ch.25] cast guard: line-of-fire measurement ARMED ({} per frame, fresh {} ms, line pad {:.0f}u, "
                         "blast pad {:.0f}u; own-ray exposure {}). Channel bounds ride CheckCast ({}).",
                         kMaxMeasuresPerFrame, kFreshMs, kLinePad, kBlastPad,
                         apmf::sightline::Armed() ? "armed" : "NOT armed -- every friendly near the line counts",
                         apmf::castgate::Installed() ? "installed" : "NOT installed: every guard claim is refused");
            return;
        }
        spdlog::warn("[ch.25] cast guard: line-of-fire measurement NOT armed -- {}. kCastGuard_LineOfFire claims are "
                     "REFUSED; channel bounds ride CheckCast ({}).",
                     g_notArmedReason.load(),
                     apmf::castgate::Installed() ? "installed" : "NOT installed: every guard claim is refused");
    }

    bool        LineOfFireArmed() { return g_armed.load(std::memory_order_acquire); }
    const char* NotArmedReason() { return g_notArmedReason.load(); }

    const char* RefuseReason(RE::FormID a_actor, const APMF_API::APMF_Param* a_param) {
        if (!apmf::castgate::Installed())
            return "the CheckCast gate (core/CastGate.cpp, 0x0A) is not installed on this runtime";
        if (!a_param) return "no param -- kIntent_CastGuard REQUIRES param.ival = CastGuardFlags (RequestEx)";
        const auto bits = static_cast<std::uint32_t>(a_param->ival);
        if ((bits & ~APMF_API::kCastGuard_All) != 0)
            return "param.ival carries a bit this APMF does not know (kCastGuard_All is 0x7 at ABI v21)";
        if ((bits & APMF_API::kCastGuard_All) == 0) return "param.ival names no guard (no CastGuardFlags bit set)";
        if (!std::isfinite(a_param->fval) || a_param->fval < 0.0f)
            return "param.fval (the TTL in seconds) is negative or not a finite number (0 = the cast default)";
        if (a_actor == 0x14) return "the actor is the player (the guard gates an NPC's own AI casts)";
        if ((bits & APMF_API::kCastGuard_LineOfFire) != 0 && !LineOfFireArmed())
            return NotArmedReason();
        return nullptr;
    }

    std::uint32_t TtlMs(float a_seconds) {
        if (!std::isfinite(a_seconds) || a_seconds <= 0.0f) return APMF_API::kCastDefaultTtlMs;
        const double ms = static_cast<double>(a_seconds) * 1000.0;
        if (ms >= static_cast<double>(APMF_API::kCastMaxTtlMs)) return APMF_API::kCastMaxTtlMs;
        return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(ms));
    }

    void Pump() {
        g_guards.clear();
        ControlMap::Get().CastGuardActors(g_guards);
        g_anyGuard.store(!g_guards.empty(), std::memory_order_relaxed);
        if (!LineOfFireArmed()) return;
        const auto now = apmf::clock::MonotonicMs();

        // Free the slots of casters no longer guarded for the line of fire (a stale measurement
        // must never answer for a later claim: it would read as fresh only within kFreshMs, but
        // the slot would also leak).
        g_live.clear();
        for (const auto& [fid, flags] : g_guards)
            if (flags & APMF_API::kCastGuard_LineOfFire) g_live.push_back(fid);
        for (auto& s : g_slots) {
            const auto k = s.key.load(std::memory_order_relaxed);
            if (k == 0) continue;
            if (std::find(g_live.begin(), g_live.end(), KeyCaster(k)) == g_live.end()) Empty(s);
        }

        if (!g_live.empty()) {
            const std::size_t n    = g_live.size();
            const std::size_t todo = std::min<std::size_t>(n, kMaxMeasuresPerFrame);
            for (std::size_t i = 0; i < todo; ++i) Measure(g_live[(g_rr + i) % n], now);
            g_rr = (g_rr + todo) % n;
        }

        if (now - g_lastHeartbeatMs >= kHeartbeatMs) {
            g_lastHeartbeatMs = now;
            if (!g_live.empty() || g_lofRefused.load(std::memory_order_relaxed) ||
                g_chanRefused.load(std::memory_order_relaxed) || g_startHeld.load(std::memory_order_relaxed))
                spdlog::info("[ch.25] HEARTBEAT {} line-of-fire caster(s): {} measure(s) ({} no aim, {} friendly on a "
                             "line, {} friendly near an aim); 0x0A refused {} (line of fire) + {} (channel bound); "
                             "0x06 held {} ({} not measured); {} stood (Engine outranked the guard); table full {}.",
                             g_live.size(), g_measures.load(), g_noAim.load(), g_lineHits.load(), g_blastNear.load(),
                             g_lofRefused.load(), g_chanRefused.load(), g_startHeld.load(), g_startUnknown.load(),
                             g_stood.load(), g_tableFull.load());
        }
    }

    void ResetAll(const char* a_why) {
        g_anyGuard.store(false, std::memory_order_relaxed);
        for (auto& s : g_slots)
            if (s.key.load(std::memory_order_relaxed) != 0) Empty(s);
        g_rr = 0;
        spdlog::info("[ch.25] cast guard measurements reset ({}).", a_why ? a_why : "?");
    }

    bool DenyCheckCast(RE::Actor* a_actor, RE::FormID a_fid, RE::MagicItem* a_spell, allowance::Hand a_hand) {
        if (!g_anyGuard.load(std::memory_order_relaxed) || !a_spell || a_hand == allowance::Hand::kUnknown) return false;
        std::uint32_t    flags = 0;
        APMF_API::Handle h     = APMF_API::kInvalidHandle;
        if (!ControlMap::Get().TryGetCastGuard(a_fid, flags, h) || flags == 0) return false;

        // CHANNEL BOUND: the AI's own concentration spell on a bounded hand, unless the client's
        // own live cast claim on that hand names it.
        const std::uint32_t handBit = a_hand == allowance::Hand::kLeft ? APMF_API::kCastGuard_ChannelLeft
                                                                       : APMF_API::kCastGuard_ChannelRight;
        if ((flags & handBit) != 0 && ConcentrationSpell(a_spell) &&
            !allowance::CastClaimNamesForHand(a_fid, a_spell->GetFormID(), a_hand)) {
            const bool stands = Decide(h, a_fid, a_actor, a_spell, "CheckCast (0x0A)", "channel bound",
                                       kReasonChannel,
                                       fmt::format("a concentration spell on the {} hand with no live cast claim "
                                                   "naming it there",
                                                   a_hand == allowance::Hand::kLeft ? "left" : "right"));
            if (stands) {
                g_chanRefused.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }

        // LINE OF FIRE: refused only on a FRESH measurement that says blocked (a NO here at the
        // release or on a channel tick interrupts the cast, so an unknown never does).
        if ((flags & APMF_API::kCastGuard_LineOfFire) != 0 && Offensive(a_spell)) {
            const auto now = apmf::clock::MonotonicMs();
            const Lof  l   = Read(a_fid);
            if (l.found && l.aim != 0 && l.measuredMs != 0 && now >= l.measuredMs && now - l.measuredMs <= kFreshMs) {
                const float area  = LargestArea(a_spell);
                const bool  line  = l.lineBlocked;
                const bool  blast = !line && area > 0.0f && l.blastFriend != 0 && l.blastDist <= area + kBlastPad;
                if (line || blast) {
                    const bool stands = Decide(h, a_fid, a_actor, a_spell, "CheckCast (0x0A)", "line of fire",
                                               line ? kReasonLine : kReasonBlast, LofDetail(l, area, line, now));
                    if (stands) {
                        g_lofRefused.fetch_add(1, std::memory_order_relaxed);
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool HoldStartCast(RE::CombatController* a_cc, RE::MagicItem* a_item) {
        if (!g_anyGuard.load(std::memory_order_relaxed) || !a_cc || !a_item || !Offensive(a_item)) return false;
        auto  attPtr = a_cc->attackerHandle.get();   // NiPointer<Actor>, refcounted for this scope (CastSeats' read)
        auto* who    = attPtr.get();
        if (!who) return false;
        const RE::FormID a_fid = who->GetFormID();
        std::uint32_t    flags = 0;
        APMF_API::Handle h     = APMF_API::kInvalidHandle;
        if (!ControlMap::Get().TryGetCastGuard(a_fid, flags, h) || (flags & APMF_API::kCastGuard_LineOfFire) == 0)
            return false;
        const auto now   = apmf::clock::MonotonicMs();
        const Lof  l     = Read(a_fid);
        const bool fresh = l.found && l.measuredMs != 0 && now >= l.measuredMs && now - l.measuredMs <= kFreshMs;
        if (fresh && l.aim == 0) return false;   // no aim: no line to fire along
        const float area  = LargestArea(a_item);
        const bool  line  = fresh && l.lineBlocked;
        const bool  blast = fresh && !line && area > 0.0f && l.blastFriend != 0 && l.blastDist <= area + kBlastPad;
        if (fresh && !line && !blast) return false;
        const bool stands = Decide(h, a_fid, who, a_item, "CheckStartCast (0x06)", "line of fire",
                                   !fresh ? kReasonUnknown : line ? kReasonLine : kReasonBlast,
                                   !fresh ? std::string("not measured yet, or older than 250 ms -- held, never clear "
                                                        "by fallback")
                                          : LofDetail(l, area, line, now));
        if (stands) {
            g_startHeld.fetch_add(1, std::memory_order_relaxed);
            if (!fresh) g_startUnknown.fetch_add(1, std::memory_order_relaxed);
        }
        return stands;
    }

}
