#include "PCH.h"
#include "core/CombatMove.h"

#include "core/Allowance.h"          // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Clock.h"
#include "core/ControlMap.h"         // TryGetOwningClaimBasis (lock-free RCU read), ServeCheck
#include "core/FacetAttribution.h"   // Decide: the calling client vs the Engine source on the CombatAction facet
#include "core/Log.h"
#include "core/MainThread.h"

#include <cmath>
#include <cstring>

// Win32 INI read -- the same hand-declared extern every INI-gated file in this project uses.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ABI v38 COMBAT MOVE. See core/CombatMove.h for the engine road and the shape (a one-shot, never a claim).
namespace apmf::combatmove {

    namespace {
        using apmf::log::Hex;
        using APMF_API::Handle;

        constexpr const char* kIni       = "Data/SKSE/Plugins/APMF.ini";
        constexpr const char* kRowCtor   = "CombatMove.BGSActionData.Ctor";
        constexpr const char* kRowVtable = "CombatMove.TESActionData";
        constexpr const char* kRowDobj   = "CombatMove.BGSDefaultObjectManager.GetSingleton";

        std::atomic<bool>        g_installed{ false };
        std::atomic<const char*> g_reason{ "not installed yet (before kDataLoaded)" };
        std::atomic<bool>        g_probe{ false };            // [Probe] bCombatMove
        std::atomic<std::uint32_t> g_probeLines{ 0 };
        constexpr std::uint32_t  kProbeLineCap = 600;         // per session; the trip is logged once

        std::uintptr_t g_ctor   = 0;   // BGSActionData's default constructor (written once in Install)
        std::uintptr_t g_vtable = 0;   // the TESActionData vtable
        RE::BGSAction* g_action = nullptr;   // the Right Attack action default object (written once in Install)

        // A world generation: ResetAll bumps it, and a move posted under an older one drops itself.
        std::atomic<std::uint32_t> g_gen{ 1 };

        // Per-handle records. Leaf lock: nothing is called while it is held (no game call, no log).
        struct Rec {
            APMF_API::APMF_CombatMoveStateInfo info{};   // size/handle/msSinceChange are filled at the read
            std::uint64_t                      changedMs = 0;
            std::uint64_t                      startedMs = 0;   // Started only: when (the hit window opens here)
            bool                               read      = false;
        };
        constexpr std::size_t           kMaxRecords = 128;
        std::mutex                      g_mx;
        std::unordered_map<Handle, Rec> g_recs;
        std::uint32_t                   g_seq = 0;   // under g_mx
        // The newest start, so the hit sink skips the lock when no move can be waiting for its hit.
        std::atomic<std::uint64_t> g_lastStartMs{ 0 };

        bool Terminal(std::uint32_t s) {
            return s == APMF_API::kCombatMoveState_Started || s == APMF_API::kCombatMoveState_Refused ||
                   s == APMF_API::kCombatMoveState_Failed;
        }

        // A finished record lives until it has been READ once or the grace has passed, whichever is LATER.
        bool Expired(const Rec& a_rec, std::uint64_t a_now) {
            return Terminal(a_rec.info.state) && a_rec.read &&
                   a_now - a_rec.changedMs > APMF_API::kCombatMoveStateGraceMs;
        }

        // Under g_mx: room for one more record. Drops expired records, then (at the cap) the oldest finished
        // one, read or not. A table full of QUEUED moves refuses the new one (false).
        bool MakeRoomLocked(std::uint64_t a_now) {
            for (auto it = g_recs.begin(); it != g_recs.end();) {
                if (Expired(it->second, a_now))
                    it = g_recs.erase(it);
                else
                    ++it;
            }
            if (g_recs.size() < kMaxRecords) return true;
            auto oldest = g_recs.end();
            for (auto it = g_recs.begin(); it != g_recs.end(); ++it) {
                if (!Terminal(it->second.info.state)) continue;
                if (oldest == g_recs.end() || it->second.changedMs < oldest->second.changedMs) oldest = it;
            }
            if (oldest == g_recs.end()) return false;
            g_recs.erase(oldest);
            return true;
        }

        struct Sent {   // what the game-thread step measured, written into the record with its verdict
            std::uint32_t info         = 0;
            float         distance     = -1.0f;
            float         bearingDeg   = -1.0f;
            float         staminaSpent = 0.0f;
        };

        // Game thread: finish a record. Returns false when the record is gone (a reset since the post): the
        // caller then logs nothing more.
        bool Finish(Handle a_handle, std::uint32_t a_state, std::uint32_t a_why, Handle a_blocker, const Sent& a_sent) {
            const std::uint64_t now = apmf::clock::MonotonicMs();
            {
                std::scoped_lock lock(g_mx);
                const auto it = g_recs.find(a_handle);
                if (it == g_recs.end()) return false;
                auto& i        = it->second.info;
                i.state        = a_state;
                i.why          = a_why;
                i.blocker      = a_blocker;
                i.info        |= a_sent.info;
                i.distance     = a_sent.distance;
                i.bearingDeg   = a_sent.bearingDeg;
                i.staminaSpent = a_sent.staminaSpent;
                i.seq          = ++g_seq;
                it->second.changedMs = now;
                if (a_state == APMF_API::kCombatMoveState_Started) it->second.startedMs = now;
            }
            if (a_state == APMF_API::kCombatMoveState_Started) g_lastStartMs.store(now, std::memory_order_release);
            return true;
        }

        const char* MoveName(std::uint32_t a_move) {
            switch (a_move) {
            case APMF_API::kCombatMove_Bash:      return "bash";
            case APMF_API::kCombatMove_PowerBash: return "power bash";
            case APMF_API::kCombatMove_Block:     return "block";
            default:                              return "unknown move";
            }
        }

        const char* WhyText(std::uint32_t a_why) {
            switch (a_why) {
            case APMF_API::kCombatMoveWhy_ActorGone:      return "the actor FormID is not an actor in memory";
            case APMF_API::kCombatMoveWhy_NotReady:       return "the actor cannot act";
            case APMF_API::kCombatMoveWhy_Busy:           return "an attack is already in progress";
            case APMF_API::kCombatMoveWhy_NotAttackReady: return "the behaviour graph's IsAttackReady is not true";
            case APMF_API::kCombatMoveWhy_TargetGone:     return "the named target is not a loaded living actor";
            case APMF_API::kCombatMoveWhy_NoAttackData:   return "the actor base has no attack entry of that kind";
            case APMF_API::kCombatMoveWhy_OffenseDenied:  return "a winning kIntent_CombatAction claim denies Offense";
            case APMF_API::kCombatMoveWhy_SubstrateHeld:  return "a combat substrate holds the actor";
            case APMF_API::kCombatMoveWhy_Outranked:      return "the client does not outrank Engine on the CombatAction "
                                                                 "facet (APMF_Sources.ini / the Harbinger page)";
            case APMF_API::kCombatMoveWhy_EngineRefused:  return "the engine's action returned false";
            case APMF_API::kCombatMoveWhy_NotApplied:     return "the engine's action returned true, but the actor's attack "
                                                                 "data / attack state do not read back as this move";
            case APMF_API::kCombatMoveWhy_Error:          return "an exception inside Harbinger's game-thread step";
            default:                                      return "unknown";
            }
        }

        // The actor's current attack data (the engine's own read: process -> high -> attackData).
        RE::BGSAttackData* CurrentAttackData(RE::Actor* a_actor) {
            auto* proc = a_actor->GetActorRuntimeData().currentProcess;
            return (proc && proc->high) ? proc->high->attackData.get() : nullptr;
        }

        // ATKD flag bits as the engine's bash gather reads them (AE 0x8A3960): bit 1 = bash, bit 2 = power,
        // bit 3 = the bit that sorts an entry into the LEFT-hand pool (never part of the bash pool).
        constexpr std::uint32_t kAtkBash  = 1u << 1;
        constexpr std::uint32_t kAtkPower = 1u << 2;
        constexpr std::uint32_t kAtkLeft  = 1u << 3;

        // The actor base's entry for the move: Bash set, the left-pool bit clear, Power as the move says. Among
        // several, the highest attack chance; ties go to the lower event string (never a random draw).
        RE::BGSAttackData* PickEntry(RE::Actor* a_actor, std::uint32_t a_move) {
            auto* base = a_actor->GetActorBase();
            auto* map  = base ? static_cast<RE::BGSAttackDataForm*>(base)->attackDataMap.get() : nullptr;
            if (!map) return nullptr;
            const bool         wantPower = a_move == APMF_API::kCombatMove_PowerBash;
            RE::BGSAttackData* best      = nullptr;
            for (const auto& kv : map->attackDataMap) {
                auto* e = kv.second.get();
                if (!e) continue;
                const auto flags = static_cast<std::uint32_t>(e->data.flags.underlying());
                if (!(flags & kAtkBash) || (flags & kAtkLeft)) continue;
                if (((flags & kAtkPower) != 0) != wantPower) continue;
                if (!best || e->data.attackChance > best->data.attackChance ||
                    (e->data.attackChance == best->data.attackChance &&
                     std::strcmp(e->event.c_str(), best->event.c_str()) < 0))
                    best = e;
            }
            return best;
        }

        // The engine's start-of-attack action for `a_entry` (core/CombatMove.h "THE ENGINE ROAD"). Game thread.
        // The object lives on this stack, as the engine's own does on StartAttack's.
        bool ProcessAttackAction(RE::Actor* a_actor, const RE::BGSAttackData* a_entry) {
            alignas(RE::TESActionData) std::byte buf[sizeof(RE::TESActionData)];
            std::memset(buf, 0, sizeof(buf));
            using Ctor_t = void* (*)(void*);                  // BGSActionData::BGSActionData() (this); returns this
            using Dtor_t = void* (*)(void*, std::uint32_t);   // slot 0: destroy; flags bit 0 = also free (never set here)
            reinterpret_cast<Ctor_t>(g_ctor)(buf);
            *reinterpret_cast<std::uintptr_t*>(buf) = g_vtable;   // the engine's next write (AE 0x7F9277)
            auto* ad      = reinterpret_cast<RE::TESActionData*>(buf);
            ad->source    = RE::NiPointer<RE::TESObjectREFR>(a_actor);
            ad->unk20     = 2;                                // the priority StartAttack passes (AE 0x7F9242 `lea edx,[rbp+2]`)
            // The action form StartAttack's wrapper stores (AE 0x7F92A4..0x7F92B3: default object table[0x2016198][0] = 49 =
            // kActionRightAttack, into +0x18; 1.5.97 0x75FD85..0x75FDA4, table 0x1DF2918[0] = 49). Review F1-1 of 6078122.
            ad->action    = g_action;
            ad->animEvent = a_entry->event;
            const bool ok = ad->Process();                    // vtable slot 5 = TESActionData::Process
            (*reinterpret_cast<Dtor_t**>(buf))[0](buf, 0);    // releases the two strings and the source reference
            return ok;
        }

        // GAME THREAD (apmf::mainthread::Pump). The whole move after the call's checks.
        void Run(Handle a_handle, RE::FormID a_actor, std::uint32_t a_move, RE::FormID a_target,
                 apmf::sources::SourceId a_client) {
            Sent sent{};
            const auto refuse = [&](std::uint32_t a_why, Handle a_blocker, const std::string& a_detail) {
                if (!Finish(a_handle, APMF_API::kCombatMoveState_Refused, a_why, a_blocker, sent)) return;
                spdlog::warn("[combat-move] {} REFUSED ({} by actor 0x{}): {}{}", a_handle, MoveName(a_move), Hex(a_actor),
                             WhyText(a_why), a_detail);
            };

            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor);
            if (!actor) return refuse(APMF_API::kCombatMoveWhy_ActorGone, 0, "");
            if (actor->IsPlayerRef())
                return refuse(APMF_API::kCombatMoveWhy_ActorGone, 0, " (the player is never moved)");
            auto* proc = actor->GetActorRuntimeData().currentProcess;
            if (!actor->Is3DLoaded() || !proc || !proc->high)
                return refuse(APMF_API::kCombatMoveWhy_NotReady, 0, " (not loaded: no 3D or no high process)");
            if (actor->IsDead()) return refuse(APMF_API::kCombatMoveWhy_NotReady, 0, " (dead)");
            if (actor->IsInKillMove()) return refuse(APMF_API::kCombatMoveWhy_NotReady, 0, " (in a kill move)");
            // The engine's own bash test (AE 0x6B90F0), in its order.
            const auto* state = actor->AsActorState();
            if (state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone)
                return refuse(APMF_API::kCombatMoveWhy_Busy, 0,
                              std::format(" (attack state {})", static_cast<std::uint32_t>(state->GetAttackState())));
            static const RE::BSFixedString s_isAttackReady{ "IsAttackReady" };
            bool ready = false;
            if (!actor->GetGraphVariableBool(s_isAttackReady, ready) || !ready)
                return refuse(APMF_API::kCombatMoveWhy_NotAttackReady, 0, "");
            if (actor->GetActorRuntimeData().boolFlags.all(RE::Actor::BOOL_FLAGS::kAttackingDisabled))
                return refuse(APMF_API::kCombatMoveWhy_NotReady, 0, " (attacking is disabled on the actor)");

            if (actor->IsInCombat()) sent.info |= APMF_API::kCombatMoveInfo_InCombat;
            if (state->IsWeaponDrawn()) sent.info |= APMF_API::kCombatMoveInfo_WeaponOut;

            // A named target is checked and MEASURED, never aimed at.
            if (a_target != 0) {
                auto* tgt = RE::TESForm::LookupByID<RE::Actor>(a_target);
                if (!tgt || !tgt->Is3DLoaded() || tgt->IsDead())
                    return refuse(APMF_API::kCombatMoveWhy_TargetGone, 0, std::format(" (target 0x{})", Hex(a_target)));
                const RE::NiPoint3 a = actor->GetPosition();
                const RE::NiPoint3 t = tgt->GetPosition();
                sent.distance        = a.GetDistance(t);
                // Heading: rotation Z, 0 = +Y, clockwise. The direction to the target in the same convention.
                constexpr float kPi  = 3.14159265358979f;
                float           diff = std::atan2(t.x - a.x, t.y - a.y) - actor->GetAngleZ();
                while (diff > kPi) diff -= 2.0f * kPi;
                while (diff < -kPi) diff += 2.0f * kPi;
                sent.bearingDeg = std::isfinite(diff) ? std::fabs(diff) * (180.0f / kPi) : -1.0f;
            }

            // A deny of the Offense leaves stands against a bash, whoever filed it; so does a substrate.
            {
                APMF_API::APMF_Param p{};
                float                b     = 0.0f;
                Handle               owner = APMF_API::kInvalidHandle;
                if (apmf::ControlMap::Get().TryGetOwningClaimBasis(a_actor, APMF_API::kIntent_CombatAction, p, b, &owner) &&
                    (static_cast<std::uint32_t>(p.ival) & APMF_API::kCombatActionCat_Offense))
                    return refuse(APMF_API::kCombatMoveWhy_OffenseDenied, owner, std::format(" (h={})", owner));
                p     = {};
                owner = APMF_API::kInvalidHandle;
                if (apmf::ControlMap::Get().TryGetOwningClaimBasis(a_actor, APMF_API::kIntent_CombatSubstrate, p, b, &owner))
                    return refuse(APMF_API::kCombatMoveWhy_SubstrateHeld, owner, std::format(" (h={})", owner));
            }

            // The move competes with what the engine's combat tree would choose next: the calling client must
            // outrank the Engine source on the CombatAction facet (the test a ch.7 deny passes).
            {
                const auto v = apmf::facetattr::Decide(apmf::sources::Facet::kCombatAction, a_handle,
                                                       apmf::sources::kEngineSource, a_actor, "");
                if (!v.enforce)
                    return refuse(APMF_API::kCombatMoveWhy_Outranked, 0,
                                  std::format(" (client priority {}, Engine {})", v.claimPrio, v.srcPrio));
            }

            auto* entry = PickEntry(actor, a_move);
            if (!entry) return refuse(APMF_API::kCombatMoveWhy_NoAttackData, 0, "");
            // Hold the entry across the action: the map owns it, but nothing here may outlive a map edit.
            const RE::NiPointer<RE::BGSAttackData> hold(entry);
            const std::string  event  = entry->event.c_str();
            const auto         eflags = static_cast<std::uint32_t>(entry->data.flags.underlying());
            auto*              avo    = actor->AsActorValueOwner();
            const float        before = avo->GetActorValue(RE::ActorValue::kStamina);
            RE::BGSAttackData* was    = CurrentAttackData(actor);

            const bool ok = ProcessAttackAction(actor, entry);

            const float after   = avo->GetActorValue(RE::ActorValue::kStamina);
            sent.staminaSpent   = (std::isfinite(before) && std::isfinite(after)) ? before - after : 0.0f;
            auto*      now      = CurrentAttackData(actor);
            const auto atkState = static_cast<std::uint32_t>(actor->AsActorState()->GetAttackState());
            const auto targetNote =
                a_target != 0 ? std::format(" target 0x{} at {:.0f}u, {:.0f} deg off his heading;", Hex(a_target),
                                            sent.distance, sent.bearingDeg)
                              : std::string{ " no target named;" };
            const auto clientName = a_client == apmf::sources::kNoSource ? std::string_view{ "none" }
                                                                         : apmf::sources::Name(a_client);
            if (!ok) {
                if (!Finish(a_handle, APMF_API::kCombatMoveState_Failed, APMF_API::kCombatMoveWhy_EngineRefused, 0, sent))
                    return;
                spdlog::warn("[combat-move] {} FAILED ({} by actor 0x{} '{}', event '{}'): {}.{} stamina {:.0f}, in combat "
                             "{}, weapon out {}, attack state now {} (client '{}').",
                             a_handle, MoveName(a_move), Hex(a_actor), actor->GetName(), event,
                             WhyText(APMF_API::kCombatMoveWhy_EngineRefused), targetNote, before,
                             (sent.info & APMF_API::kCombatMoveInfo_InCombat) != 0,
                             (sent.info & APMF_API::kCombatMoveInfo_WeaponOut) != 0, atkState, clientName);
                return;
            }
            if (now != entry || atkState != static_cast<std::uint32_t>(RE::ATTACK_STATE_ENUM::kBash)) {
                if (!Finish(a_handle, APMF_API::kCombatMoveState_Failed, APMF_API::kCombatMoveWhy_NotApplied, 0, sent)) return;
                spdlog::error("[combat-move] {} FAILED ({} by actor 0x{} '{}', event '{}'): {} -- attack data is {} "
                              "(event '{}'; it was {} before), attack state {} (Bash is 6).{} Report this line.",
                              a_handle, MoveName(a_move), Hex(a_actor), actor->GetName(), event,
                              WhyText(APMF_API::kCombatMoveWhy_NotApplied), now == entry ? "the entry" : "NOT the entry",
                              now ? now->event.c_str() : "none", was == entry ? "the entry already" : "another or none",
                              atkState, targetNote);
                return;
            }
            if (!Finish(a_handle, APMF_API::kCombatMoveState_Started, APMF_API::kCombatMoveWhy_None, 0, sent)) return;
            spdlog::info("[combat-move] {} STARTED: actor 0x{} '{}' {} (event '{}', flags 0x{}, chance {:.2f}); the engine "
                         "set his attack data and the Bash attack state;{} stamina {:.0f} -> {:.0f}, in combat {} (client "
                         "'{}').",
                         a_handle, Hex(a_actor), actor->GetName(), MoveName(a_move), event, Hex(eflags),
                         entry->data.attackChance, targetNote, before, after,
                         (sent.info & APMF_API::kCombatMoveInfo_InCombat) != 0, clientName);
        }

        void Deliver(Handle a_handle, RE::FormID a_actor, std::uint32_t a_move, RE::FormID a_target,
                     apmf::sources::SourceId a_client, std::uint32_t a_gen) {
            if (g_gen.load(std::memory_order_acquire) != a_gen) {
                spdlog::info("[combat-move] {} dropped unrun: the world changed since the request (load / new game).",
                             a_handle);
                return;
            }
            try {
                Run(a_handle, a_actor, a_move, a_target, a_client);
            } catch (...) {
                if (Finish(a_handle, APMF_API::kCombatMoveState_Failed, APMF_API::kCombatMoveWhy_Error, 0, Sent{}))
                    spdlog::error("[combat-move] {} FAILED ({} by actor 0x{}): {}.", a_handle, MoveName(a_move),
                                  Hex(a_actor), WhyText(APMF_API::kCombatMoveWhy_Error));
            }
        }

        // ---- the hit sink (the thread that dispatches the hit; the leaf mutex, FormIDs, flags and atomics only) ----
        // [Probe] bCombatMove: one line per melee hit by or on a player teammate. The hit's thread is not proven to be
        // the game thread, so NOTHING of an actor is dereferenced there (review F1-2 of 6078122): the sink keeps the two
        // FormIDs, the flags and the weapon, and the actors are looked up and read on the game thread, one Pump later.
        // The attacker's attack data and state are therefore AS OF THAT PUMP, which the line says.
        void ProbeLine(RE::FormID a_cause, RE::FormID a_target, std::uint8_t a_flags, RE::FormID a_weapon, std::uint32_t a_gen) {
            if (g_gen.load(std::memory_order_acquire) != a_gen) return;   // the world changed: the ids mean nothing now
            auto* ca = a_cause != 0 ? RE::TESForm::LookupByID<RE::Actor>(a_cause) : nullptr;
            auto* ta = a_target != 0 ? RE::TESForm::LookupByID<RE::Actor>(a_target) : nullptr;
            const bool causeMate  = ca && !ca->IsPlayerRef() && ca->IsPlayerTeammate();
            const bool targetMate = ta && !ta->IsPlayerRef() && ta->IsPlayerTeammate();
            if (!causeMate && !targetMate) return;
            const std::uint32_t n = g_probeLines.fetch_add(1, std::memory_order_relaxed);
            if (n >= kProbeLineCap) {
                if (n == kProbeLineCap)
                    spdlog::info("[combat-move-probe] line cap ({}) reached for this session: no more hit lines.", kProbeLineCap);
                return;
            }
            std::string   atkEvent = "none";
            std::uint32_t atkFlags = 0, atkState = 0;
            if (ca) {
                if (auto* d = CurrentAttackData(ca)) {
                    atkEvent = d->event.c_str();
                    atkFlags = static_cast<std::uint32_t>(d->data.flags.underlying());
                }
                atkState = static_cast<std::uint32_t>(ca->AsActorState()->GetAttackState());
            }
            bool wantBlock = false, graphBlock = false, graphRead = false;
            if (ta) {
                wantBlock = ta->AsActorState()->actorState2.wantBlocking != 0;
                static const RE::BSFixedString s_isBlocking{ "IsBlocking" };
                graphRead = ta->GetGraphVariableBool(s_isBlocking, graphBlock);
            }
            const auto bit = [a_flags](RE::TESHitEvent::Flag f) { return (a_flags & static_cast<std::uint8_t>(f)) != 0; };
            spdlog::info("[combat-move-probe] HIT cause 0x{} '{}'{} -> target 0x{} '{}'{} | flags power={} bash={} blocked={} "
                         "sneak={} weapon 0x{} | one frame later: attacker attack data '{}' flags 0x{} (bash bit {}, power bit "
                         "{}), attack state {} | target wantBlocking={} graph IsBlocking={}",
                         Hex(a_cause), ca ? ca->GetName() : "", causeMate ? " [teammate]" : "", Hex(a_target),
                         ta ? ta->GetName() : "", targetMate ? " [teammate]" : "", bit(RE::TESHitEvent::Flag::kPowerAttack),
                         bit(RE::TESHitEvent::Flag::kBashAttack), bit(RE::TESHitEvent::Flag::kHitBlocked),
                         bit(RE::TESHitEvent::Flag::kSneakAttack), Hex(a_weapon), atkEvent, Hex(atkFlags),
                         (atkFlags & kAtkBash) != 0, (atkFlags & kAtkPower) != 0, atkState, wantBlock,
                         graphRead ? (graphBlock ? "true" : "false") : "unreadable");
        }

        // The hit's thread: PODs only, then one Post. Bounded: no Post once the line cap is reached.
        void ProbeHit(const RE::TESHitEvent* a_e, const RE::TESObjectREFR* a_cause, const RE::TESObjectREFR* a_target) {
            if (a_e->projectile != 0) return;   // a projectile hit is not a melee move
            if (g_probeLines.load(std::memory_order_relaxed) > kProbeLineCap) return;
            const RE::FormID    cause  = a_cause ? a_cause->GetFormID() : 0;
            const RE::FormID    target = a_target ? a_target->GetFormID() : 0;
            const auto          flags  = static_cast<std::uint8_t>(a_e->flags.underlying());
            const RE::FormID    weapon = a_e->source;
            const std::uint32_t gen    = g_gen.load(std::memory_order_acquire);
            apmf::mainthread::Post([cause, target, flags, weapon, gen] {
                try {
                    ProbeLine(cause, target, flags, weapon, gen);
                } catch (...) {
                }
            });
        }

        class HitSink final : public RE::BSTEventSink<RE::TESHitEvent> {
        public:
            static HitSink* Get() {
                static HitSink s;
                return &s;
            }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_e, RE::BSTEventSource<RE::TESHitEvent>*) override {
                try {
                    if (!a_e) return RE::BSEventNotifyControl::kContinue;
                    auto* cause  = a_e->cause.get();
                    auto* target = a_e->target.get();
                    if (g_probe.load(std::memory_order_relaxed)) ProbeHit(a_e, cause, target);
                    if (!cause || !a_e->flags.any(RE::TESHitEvent::Flag::kBashAttack))
                        return RE::BSEventNotifyControl::kContinue;
                    const std::uint64_t now  = apmf::clock::MonotonicMs();
                    const std::uint64_t last = g_lastStartMs.load(std::memory_order_acquire);
                    if (last == 0 || now - last > APMF_API::kCombatMoveHitWindowMs) return RE::BSEventNotifyControl::kContinue;
                    const RE::FormID fid   = cause->GetFormID();
                    const RE::FormID tfid  = target ? target->GetFormID() : 0;
                    Handle           found = APMF_API::kInvalidHandle;
                    bool             onTarget = false;
                    {
                        std::scoped_lock lock(g_mx);
                        // The OLDEST started move of this actor still waiting for its hit takes it.
                        Rec* pick = nullptr;
                        for (auto& [h, r] : g_recs) {
                            if (r.info.actor != fid || r.info.state != APMF_API::kCombatMoveState_Started) continue;
                            if (r.info.info & APMF_API::kCombatMoveInfo_HitSeen) continue;
                            if (now < r.startedMs || now - r.startedMs > APMF_API::kCombatMoveHitWindowMs) continue;
                            if (!pick || r.startedMs < pick->startedMs) {
                                pick  = &r;
                                found = h;
                            }
                        }
                        if (pick) {
                            pick->info.info |= APMF_API::kCombatMoveInfo_HitSeen;
                            if (a_e->flags.any(RE::TESHitEvent::Flag::kHitBlocked))
                                pick->info.info |= APMF_API::kCombatMoveInfo_HitBlocked;
                            onTarget = pick->info.target != 0 && pick->info.target == tfid;
                            if (onTarget) pick->info.info |= APMF_API::kCombatMoveInfo_HitOnTarget;
                            pick->info.hitTarget = tfid;
                            pick->info.seq       = ++g_seq;
                            pick->changedMs      = now;
                        }
                    }
                    if (found != APMF_API::kInvalidHandle)
                        spdlog::info("[combat-move] {} HIT: actor 0x{}'s bash landed on 0x{}{}{}.", found, Hex(fid), Hex(tfid),
                                     onTarget ? " (the named target)" : "",
                                     a_e->flags.any(RE::TESHitEvent::Flag::kHitBlocked) ? ", blocked" : "");
                } catch (...) {
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void Install() {
        if (g_installed.load(std::memory_order_relaxed)) return;
        const auto no = [](const char* a_why, bool a_error) {
            g_reason.store(a_why);
            if (a_error)
                spdlog::error("[combat-move] NOT installed -- {}. Every kIntent_CombatMove request is refused.", a_why);
            else
                spdlog::warn("[combat-move] NOT installed -- {}. Every kIntent_CombatMove request is refused.", a_why);
        };
        if (GetPrivateProfileIntA("CombatMove", "bCombatMove", 1, kIni) == 0) return no("[CombatMove] bCombatMove=0", false);
        if (REL::Module::IsVR()) return no("VR runtime (the action road was read on 1.6.1170 and 1.5.97 only)", false);
        // CLAUDE.md rule 11: exactly the two builds whose StartAttack / mediator road was read. Never a family
        // test, and no id of this file is resolved before this gate (1.7.104 is refused here by name).
        if (!REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) && !REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97))
            return no("runtime not verified for the combat move (only 1.6.1170 and 1.5.97 are; 1.7.104 is not yet)", false);

        const std::uintptr_t ctor = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(15916, 41558) }.address();
        const std::uintptr_t vt   = REL::Relocation<std::uintptr_t>{ RE::VTABLE_TESActionData[0] }.address();
        bool                 ok   = apmf::allowance::SeatVerified(ctor, kRowCtor);
        ok                        = apmf::allowance::SeatVerified(vt, kRowVtable) && ok;
        ok = apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(10878, 13894) }.address(), kRowDobj) && ok;
        if (!ok)
            return no("the mit-3.7 self-check refused a row (CombatMove.BGSActionData.Ctor / CombatMove.TESActionData / "
                      "CombatMove.BGSDefaultObjectManager.GetSingleton)", true);
        // The action form of the engine's own start-of-attack: the Right Attack default object, through the fork's
        // exact-build accessor (index 49 is below every entry 1.7.104 inserts), RTTI-checked. kDataLoaded: it is loaded.
        auto*              dobj = RE::BGSDefaultObjectManager::GetSingleton();
        RE::TESForm* const raw  = dobj ? dobj->GetObject(RE::DEFAULT_OBJECT::kActionRightAttack) : nullptr;
        auto* const        act  = raw ? raw->As<RE::BGSAction>() : nullptr;
        if (!act) return no("the Right Attack action default object did not resolve to a BGSAction", true);
        g_action = act;
        g_ctor   = ctor;
        g_vtable = vt;

        g_probe.store(GetPrivateProfileIntA("Probe", "bCombatMove", 0, kIni) != 0, std::memory_order_relaxed);

        // The hit sink. The holder's GetSingleton is the row the substrate's and ch.29's sinks already verify.
        bool sink = false;
        if (apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(14108, 14298) }.address(),
                                          "Travel.ScriptEventSourceHolder.GetSingleton (the hit / combat sinks)")) {
            if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                holder->AddEventSink<RE::TESHitEvent>(HitSink::Get());
                sink = true;
            }
        }
        if (!sink)
            spdlog::error("[combat-move] the hit sink could not be registered: kCombatMoveInfo_HitSeen is never set and the "
                          "[Probe] bCombatMove lines are not written. The move itself is unaffected. Report this line.");

        g_reason.store("installed");
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[combat-move] installed (kIntent_CombatMove, ABI v38): one engine start-of-attack action per request "
                     "(bash / power bash), game thread; action form 0x{} (Right Attack); hit sink {}; [Probe] bCombatMove={}.",
                     Hex(act->GetFormID()), sink ? "registered" : "MISSING", g_probe.load() ? 1 : 0);
    }

    bool Installed() { return g_installed.load(std::memory_order_acquire); }

    const char* NotInstalledReason() { return g_reason.load(); }

    bool Enqueue(Handle a_handle, RE::FormID a_actor, const APMF_API::APMF_Param* a_param,
                 apmf::sources::SourceId a_client, ServeCheck* a_dry) {
        // ABI v35 (CanServe): in CHECK mode each refusal below records itself and returns before its log.
        const auto dryRefuse = [a_dry](std::uint32_t a_code, const char* a_why) {
            a_dry->Refuse(a_code, a_why);
            return false;
        };
        if (!Installed()) {
            if (a_dry) return dryRefuse(APMF_API::kServeRefusal_NotInstalled, NotInstalledReason());
            spdlog::warn("[combat-move] request REFUSED (actor 0x{}): not installed -- {}.", Hex(a_actor),
                         NotInstalledReason());
            return false;
        }
        if (!a_param) {
            if (a_dry)
                return dryRefuse(APMF_API::kServeRefusal_BadRequest, "no param (kIntent_CombatMove needs param.ival = a CombatMove)");
            spdlog::warn("[combat-move] request REFUSED (actor 0x{}): no param. kIntent_CombatMove needs RequestEx with "
                         "param.ival = a CombatMove.", Hex(a_actor));
            return false;
        }
        const auto move = static_cast<std::uint32_t>(a_param->ival);
        if (move == APMF_API::kCombatMove_Block) {
            if (a_dry)
                return dryRefuse(APMF_API::kServeRefusal_BadRequest, "kCombatMove_Block is reserved: not built in ABI v38");
            spdlog::warn("[combat-move] request REFUSED (actor 0x{}): kCombatMove_Block is reserved, not built in ABI v38.",
                         Hex(a_actor));
            return false;
        }
        if (move != APMF_API::kCombatMove_Bash && move != APMF_API::kCombatMove_PowerBash) {
            if (a_dry) return dryRefuse(APMF_API::kServeRefusal_BadRequest, "param.ival is not a CombatMove this ABI builds");
            spdlog::warn("[combat-move] request REFUSED (actor 0x{}): param.ival {} is not a CombatMove this ABI builds.",
                         Hex(a_actor), a_param->ival);
            return false;
        }
        if (a_actor == 0 || a_actor == 0x14) {
            if (a_dry)
                return a_actor == 0 ? dryRefuse(APMF_API::kServeRefusal_BadRequest, "actor 0")
                                    : dryRefuse(APMF_API::kServeRefusal_Player, "the player is never moved");
            spdlog::warn("[combat-move] request REFUSED: actor 0x{} -- {}.", Hex(a_actor),
                         a_actor == 0 ? "actor 0" : "the player is never moved");
            return false;
        }
        if (a_param->target == a_actor) {
            if (a_dry) return dryRefuse(APMF_API::kServeRefusal_BadRequest, "param.target is the actor itself");
            spdlog::warn("[combat-move] request REFUSED (actor 0x{}): param.target is the actor itself.", Hex(a_actor));
            return false;
        }

        if (a_dry) {   // every call-time check but the room check passed; the rest is the game thread's
            a_dry->pending = APMF_API::kServePending_OneShot | APMF_API::kServePending_CombatMoveRoom;
            return true;
        }
        // NO FORM LOOKUP HERE: the caller's thread. Everything that needs a form is decided in Run.
        const RE::FormID target = a_param->target;
        std::uint32_t    gen    = 0;
        bool             room   = false;
        {
            std::scoped_lock    lock(g_mx);
            gen                     = g_gen.load(std::memory_order_acquire);   // under g_mx: ResetAll bumps it there
            const std::uint64_t now = apmf::clock::MonotonicMs();
            room = MakeRoomLocked(now);
            if (room) {
                Rec r{};
                r.info.state      = APMF_API::kCombatMoveState_Queued;
                r.info.actor      = a_actor;
                r.info.target     = target;
                r.info.move       = move;
                r.info.distance   = -1.0f;
                r.info.bearingDeg = -1.0f;
                r.info.seq        = ++g_seq;
                r.changedMs       = now;
                g_recs[a_handle]  = r;
            }
        }
        if (!room) {
            spdlog::warn("[combat-move] request REFUSED (actor 0x{}): {} moves are already queued.", Hex(a_actor), kMaxRecords);
            return false;
        }

        apmf::mainthread::Post([a_handle, a_actor, move, target, a_client, gen] {
            Deliver(a_handle, a_actor, move, target, a_client, gen);
        });
        return true;
    }

    std::uint32_t GetState(Handle a_handle, APMF_API::APMF_CombatMoveStateInfo* a_out) {
        APMF_API::APMF_CombatMoveStateInfo info{};
        info.size       = sizeof(info);
        info.state      = APMF_API::kCombatMoveState_Unknown;
        info.handle     = a_handle;
        info.distance   = -1.0f;
        info.bearingDeg = -1.0f;
        if (a_handle != APMF_API::kInvalidHandle) {
            const std::uint64_t now = apmf::clock::MonotonicMs();
            std::scoped_lock    lock(g_mx);
            if (const auto it = g_recs.find(a_handle); it != g_recs.end()) {
                const std::uint64_t age = now - it->second.changedMs;
                if (!Expired(it->second, now)) {
                    if (Terminal(it->second.info.state)) it->second.read = true;   // the grace now counts
                    info               = it->second.info;
                    info.size          = sizeof(info);
                    info.handle        = a_handle;
                    info.msSinceChange = age > 0xFFFFFFFEull ? 0xFFFFFFFEu : static_cast<std::uint32_t>(age);
                }
            }
        }
        if (a_out && a_out->size >= APMF_API::kCombatMoveStateInfoV38Size) {
            const std::uint32_t callerSize = a_out->size;
            std::memcpy(a_out, &info, APMF_API::kCombatMoveStateInfoV38Size);
            a_out->size = callerSize;
        }
        return info.state;
    }

    void ResetAll(const char* a_why) {
        std::size_t n = 0;
        {
            std::scoped_lock lock(g_mx);
            g_gen.fetch_add(1, std::memory_order_acq_rel);   // under g_mx, so no record is filed under the old one
            n = g_recs.size();
            g_recs.clear();
        }
        g_lastStartMs.store(0, std::memory_order_release);
        g_probeLines.store(0, std::memory_order_relaxed);
        if (n != 0) spdlog::info("[combat-move] {} -- forgot {} move record(s).", a_why, n);
    }

}
