#include "PCH.h"
#include "channels/AnchorGuard.h"

#include "channels/CombatEntry.h"        // hold::StopCombatRanked: the ranked end of a leaked fight
#include "channels/CombatSubstrate.h"    // StateOf / AnyLive / AnchorId / RemoveAnchorFromGroup
#include "core/Allowance.h"              // SeatVerified, IsRuntime1_7_104
#include "core/Clock.h"
#include "core/FacetAttribution.h"       // CallerBelow: StartCombat's caller (the seat's stack scan, as ch.22's)
#include "core/Log.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// THE ANCHOR GUARD (SEV-1, field LoreRim 2026-10-19 14:28; INVARIANTS #0 (j)).
//
// WHAT HAPPENED. A teleport put Sven (NOT a substrate holder) beside Auri, a dry-run holder whose anchor stood
// 72 u behind her. 2.8 s later Sven was in real engine combat with the anchor as his only target (foes 0 for
// 10 s, battle music, Glass Battleaxe drawn, swings at the invisible anchor, townsfolk reacting). The anchor-
// target refusal (StartCombat's TARGET check, ch.22's second site) counted 0 refusals: he did not enter through
// StartCombat(anchor). Auri's TTL CLOSE stopped only Auri.
//
// THE ROUTE (disassembly of both unpacked images, 2026-10-19; agent log apmf-anchor-leak.md). Actor::StartCombat
// (AE id 38561 0x6B6930 / SE 37608 0x6251B0) takes (this rcx, target rdx, group r8) and copies them at AE +0x23 /
// SE +0x24: `mov rbp, r8; mov rsi, rdx; mov rdi, rcx` (49 8B E8 48 8B F2 48 8B F9, verified row
// AnchorGuard.StartCombat.TargetGroupRegs). Nothing writes rbp / rsi / rdi again before the self check (AE
// 0x6B69BF, return 0x6B69CA; SE 0x62523D, return 0x625248): the instructions between write only r9, rax, rcx,
// r15, ebx, eax and the TLS word. The TARGET check runs only for a non-null target (AE 0x6B69F7 / SE 0x625275
// `test rsi,rsi; je`), and with a group StartCombat takes the JOIN path whatever the target (AE 0x6B6CA5
// `test rbp,rbp` -> 0x83D2C0 with r8 = rbp; SE the same shape). The caller AE 0x71C2DD (function 0x71C020) /
// SE 0x68848E (function 0x688270), the 8th of the ten StartCombat callers on both runtimes, calls it with a NULL
// target exactly when it has a group: `test rsi,rsi; jne ...; test r15,r15; je exit` (AE 0x71C1C2 / SE
// 0x688368), then `mov r8,r15; mov rdx,rsi; mov rcx,rdi; call StartCombat` -- "join this group with no target",
// which inherits the group's targets. In a substrate group that target is the anchor. NOT PROVEN to be the call
// that ran on Sven (no line named it); the entry-route trace below names the caller of every StartCombat while
// a substrate is live, so the next field cycle proves or refutes it.
//
// THE SEAT (closes that route). ch.22's slot-0x99 stub is WIDENED (channels/CombatReentryDeny.cpp
// BuildRdiStub): at StartCombat's self-check return address it hands the thunk r8 = rsi (the target) and
// r9 = rbp (the group); everywhere else r8 = rdi as before. AnswerJoin REFUSES (IsDead answers "dead";
// StartCombat returns false before its spinlock, before anything) when ALL hold:
//   * the target is NULL (a join that takes the group's targets; a non-null target either is the anchor -- the
//     TARGET check refuses it -- or is a real foe, which makes a holder REAL by design 3.6a: not denied);
//   * the group is a SUBSTRATE GROUP: its address is in the set the sweep published this frame -- a group with
//     no target but the anchor that has a live (held) holder member or lists the anchor. ADDRESS COMPARE ONLY:
//     a CombatGroup has no reference count and the seat runs on any thread, so the seat never reads through it;
//   * the actor is not a live holder (combatsubstrate::StateOf: Held or Real passes, as RefuseAnchorTarget).
// Known limit of the address set: it is one frame old. A group the engine creates and joins inside one frame is
// not in it yet (the sweep is the backstop); a freed group's address reused within that frame could refuse an
// unrelated null-target join once (logged as a refusal, never silent).
//
// THE SWEEP (route-independent backstop, principle 2). Game thread, every Poll while any substrate is live and
// kTailMs after the last one closes. Walks the HIGH-process actors (ProcessLists::ForEachHighActor), reads each
// controller's target and its group's targets / members under the group's READ lock (BSReadLockGuard, the
// PartyPoll / AfterLoad rule; handles are copied out, resolved after the lock), and keeps NO group pointer past
// the frame. An actor that is not a live holder and targets the anchor, or whose group lists it, is a LEAK:
//   (a) an ERROR line (never masked; repeats for the same actor are summarised every kLogEveryMs);
//   (b) the anchor out of its group (CombatGroup::RemoveTarget, the substrate's verified row) -- ONLY when no
//       live holder is a member of that group. In a group a held holder shares, removing the anchor would end
//       that holder's declared substrate; there the non-holder leaves the group instead, through (c);
//   (c) the held engagement's RANKED StopCombat (combatentry::hold::StopCombatRanked) when nothing but the anchor
//       is left for it to fight. A group with a real target is a real fight: nothing is ended (a holder in it
//       drops on trigger (a)).
// CLOSE (CloseSub) reads the closing holder's group members before its StopCombat and runs (a)-(c) on every
// member that is not a live holder after it. The player is logged, never acted on.
//
// THREADING. AnswerJoin: any thread; reads the published atomics, StateOf (the lock-free slot table) and the
// two actors' FormIDs; its log state sits under g_logMx (no engine call under it; only spdlog's own lock nests
// inside it, and spdlog never calls back into this file).
// Everything else: game thread only (Poll / CloseSub / ResetAll).
// ============================================================================

namespace {

    using apmf::log::Hex;
    namespace subs = apmf::combatsubstrate;

    constexpr const char*   kIni         = "Data/SKSE/Plugins/APMF.ini";
    constexpr std::uint64_t kTailMs      = 10000;   // the sweep runs this long after the last substrate closed
    constexpr std::uint64_t kRetryMs     = 500;     // one cleanup attempt per actor per this
    constexpr std::uint64_t kLogEveryMs  = 5000;    // a still-leaking actor's lines repeat at most this often
    constexpr std::size_t   kMaxGroups   = 32;      // published substrate groups
    constexpr std::uint64_t kTraceWinMs  = 10000;   // the entry trace's rate window
    constexpr std::uint32_t kTraceMax    = 40;      // lines per window
    constexpr std::uint64_t kRefuseLogMs = 2000;    // a repeated refusal of the same (actor, group) logs this often

    // StartCombat's register copies (verified row AnchorGuard.StartCombat.TargetGroupRegs).
    constexpr std::uintptr_t kRegsOffsetAE = 0x23;   // 0x6B6953 - 0x6B6930
    constexpr std::uintptr_t kRegsOffsetSE = 0x24;   // 0x6251D4 - 0x6251B0

    enum : std::uint8_t { kKindHeld = 1u << 0, kKindListed = 1u << 1 };

    std::atomic<bool>        g_verified{ false };   // InstallSeat passed
    std::atomic<bool>        g_seatOn{ false };     // the widened stub is in the slot
    std::atomic<const char*> g_seatState{ "not installed (before kDataLoaded)" };
    bool                     g_trace = true;        // [CombatSubstrate] bSubstrateEntryTrace

    // The published substrate groups: written by the sweep (game thread), read by the seat (any thread).
    std::array<std::atomic<std::uintptr_t>, kMaxGroups> g_groups{};
    std::array<std::atomic<std::uint8_t>, kMaxGroups>   g_kinds{};
    std::atomic<std::uint64_t>                          g_refusals{ 0 };

    std::uint8_t GroupKind(const void* a_g) {
        const auto p = reinterpret_cast<std::uintptr_t>(a_g);
        if (p == 0) return 0;
        for (std::size_t i = 0; i < kMaxGroups; ++i)
            if (g_groups[i].load(std::memory_order_acquire) == p) return g_kinds[i].load(std::memory_order_relaxed);
        return 0;
    }

    std::string KindText(std::uint8_t a_k) {
        if (!a_k) return "not a substrate group";
        std::string s;
        if (a_k & kKindHeld) s = "has a held holder member";
        if (a_k & kKindListed) s += s.empty() ? "lists the anchor" : ", lists the anchor";
        return s;
    }

    // ---- the seat's log state (any thread, leaf mutex) -------------------------------------------
    struct TraceKey {
        RE::FormID     target = 0;
        std::uintptr_t group = 0, caller = 0;
        bool operator==(const TraceKey&) const = default;
    };
    std::mutex                                     g_logMx;   // no engine call under it (spdlog's lock only)
    std::unordered_map<RE::FormID, TraceKey>       g_traceLast;
    std::uint64_t                                  g_traceWinStart = 0;
    std::uint32_t                                  g_traceLines = 0, g_traceSuppressed = 0;
    std::unordered_map<RE::FormID, std::uint64_t>  g_refuseLast;   // (actor ^ group) -> last log ms

    // ---- the sweep's state (game thread) -------------------------------------------------------------
    struct LeakState {
        std::uint64_t lastTryMs = 0, lastLogMs = 0;
        std::uint32_t seen = 0, unlogged = 0;
    };
    std::unordered_map<RE::FormID, LeakState> g_leaks;
    bool          g_sweeping   = false;
    std::uint64_t g_tailUntil  = 0;
    std::uint64_t g_leakTotal  = 0;   // leaked actors treated this sweep window

    bool LiveHolder(RE::FormID a_id) {
        const auto st = subs::StateOf(a_id);
        return st == APMF_API::kSubstrateState_Held || st == APMF_API::kSubstrateState_Real;
    }

    // What the group holds, read under its READ lock; handles copied out, resolved after it.
    struct GroupRead {
        bool          listed = false;   // the anchor is a group target
        std::uint32_t other = 0;        // targets that are not the anchor
        std::uint32_t targets = 0;
        std::vector<RE::ActorHandle> members;
    };
    GroupRead ReadGroup(RE::CombatGroup* a_g, std::uint32_t a_anchorH, bool a_wantMembers) {
        GroupRead r;
        if (!a_g) return r;
        RE::BSReadLockGuard guard(a_g->lock);
        r.targets = a_g->targets.size();
        for (const auto& t : a_g->targets) {
            const auto h = t.targetHandle.native_handle();
            if (a_anchorH != 0 && h == a_anchorH) r.listed = true;
            else if (h != 0) ++r.other;
        }
        if (a_wantMembers) {
            r.members.reserve(a_g->members.size());
            for (const auto& m : a_g->members) r.members.push_back(m.memberHandle);
        }
        return r;
    }

    bool AnyHeldMember(const std::vector<RE::ActorHandle>& a_members, RE::FormID a_except) {
        for (const auto& mh : a_members) {
            const auto mp = mh.get();
            const auto* m = mp.get();
            if (m && m->GetFormID() != a_except && subs::StateOf(m->GetFormID()) == APMF_API::kSubstrateState_Held)
                return true;
        }
        return false;
    }

    // (a)-(c) on one actor that is not a live holder. Fresh reads (no pointer from an earlier frame).
    void Treat(RE::Actor* a_actor, RE::Actor* a_anchor, const std::string& a_ctx, std::uint64_t a_now, bool a_force) {
        if (!a_actor || !a_anchor || a_actor == a_anchor) return;
        const RE::FormID id = a_actor->GetFormID();
        if (LiveHolder(id)) return;
        auto& ls = g_leaks[id];
        if (!a_force && ls.lastTryMs != 0 && a_now - ls.lastTryMs < kRetryMs) return;
        auto* cc = a_actor->GetActorRuntimeData().combatController;
        if (!cc) return;
        const std::uint32_t anchorH  = a_anchor->GetHandle().native_handle();
        const bool          tAnchor  = cc->targetHandle.native_handle() == anchorH;
        RE::CombatGroup*    g        = cc->combatGroup;
        GroupRead           gr       = ReadGroup(g, anchorH, true);
        if (!tAnchor && !gr.listed) return;   // clean
        ls.lastTryMs = a_now;
        ++ls.seen;
        const bool held = AnyHeldMember(gr.members, id);
        const bool logNow = ls.lastLogMs == 0 || a_now - ls.lastLogMs >= kLogEveryMs;
        if (logNow) {
            ++g_leakTotal;
            spdlog::error("[substrate] ANCHOR LEAK ({}): 0x{} '{}' is NOT a substrate holder and {} -- group 0x{} ({} "
                          "member(s){}, {} target(s), {} not the anchor){}. A deny hole, not a fallback: cleaning up.",
                          a_ctx, Hex(id), a_actor->GetName(),
                          tAnchor ? (gr.listed ? "its controller targets the anchor, which its group lists"
                                               : "its controller targets the anchor")
                                  : "its group lists the anchor",
                          Hex(reinterpret_cast<std::uintptr_t>(g), 1), gr.members.size(),
                          held ? ", a held holder among them" : "", gr.targets, gr.other,
                          ls.unlogged ? fmt::format("; {} unlogged sighting(s) since the last line", ls.unlogged)
                                      : std::string());
            ls.unlogged = 0;
        } else {
            ++ls.unlogged;
        }
        std::string did;
        const RE::FormID anchorId = a_anchor->GetFormID();
        if (a_actor->IsPlayerRef()) {
            did = "the player: logged only";
        } else if (gr.other != 0) {
            did = "the group also has a real target: that fight is the engine's (a holder in it drops on real combat); "
                  "nothing ended";
        } else if (held) {
            did = "a held holder shares the group: the anchor stays in it (the holder's substrate stands); " +
                  apmf::combatentry::hold::StopCombatRanked(a_actor, APMF_API::kInvalidHandle, anchorId,
                                                             a_anchor->GetHandle());
        } else {
            std::uint32_t left = gr.targets;
            if (gr.listed && g) {
                subs::RemoveAnchorFromGroup(g);
                const GroupRead after = ReadGroup(g, anchorH, false);
                left = after.targets;
                did = fmt::format("anchor removed from the group (RemoveTarget){}, {} target(s) left",
                                  after.listed ? " -- STILL LISTED" : "", left);
            } else {
                did = "the group does not list the anchor (its controller still names it)";
            }
            if (left == 0 || (!gr.listed && gr.other == 0)) {
                did += "; " + apmf::combatentry::hold::StopCombatRanked(a_actor, APMF_API::kInvalidHandle, anchorId,
                                                                      a_anchor->GetHandle());
            }
        }
        if (logNow) {
            ls.lastLogMs = a_now;
            spdlog::error("[substrate] ANCHOR LEAK 0x{} cleanup: {}.", Hex(id), did);
        }
    }

}

namespace apmf::anchorguard {

    bool InstallSeat(std::uintptr_t a_startCombat) {
        const char* why = nullptr;
        char        b[16]{};
        REX::W32::GetPrivateProfileStringA("CombatSubstrate", "bSubstrateEntryTrace", "1", b, sizeof(b), kIni);
        g_trace = std::atoi(b) != 0;
        std::memset(b, 0, sizeof(b));
        REX::W32::GetPrivateProfileStringA("CombatSubstrate", "bSubstrateJoinGuard", "1", b, sizeof(b), kIni);
        const bool on = std::atoi(b) != 0;
        if (apmf::allowance::IsRuntime1_7_104()) {
            why = "refused by name on 1.7.104 (its StartCombat register copies are not disassembled here)";
        } else if (!REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) && !REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97)) {
            why = "the runtime is not exactly 1.6.1170 or 1.5.97";
        } else if (!on) {
            why = "[CombatSubstrate] bSubstrateJoinGuard=0 (the null-target join into a substrate group is NOT refused; "
                  "the sweep still cleans it up)";
        } else {
            const std::uintptr_t site =
                a_startCombat + (REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170) ? kRegsOffsetAE : kRegsOffsetSE);
            if (!apmf::allowance::SeatVerified(site, "AnchorGuard.StartCombat.TargetGroupRegs"))
                why = "the self-check refused StartCombat's register-copy row (AnchorGuard.StartCombat.TargetGroupRegs)";
        }
        if (why) {
            g_seatState.store(why, std::memory_order_release);
            spdlog::warn("[substrate] anchor JOIN GUARD seat NOT installed -- {}. A null-target StartCombat into a "
                         "substrate group is not refused at its source; the anchor-leak sweep still runs.",
                         why);
            return false;
        }
        g_verified.store(true, std::memory_order_release);
        g_seatState.store("verified, waiting for the slot write", std::memory_order_release);
        return true;
    }

    void SeatArmed() {
        if (!g_verified.load(std::memory_order_acquire)) return;
        g_seatState.store("ON", std::memory_order_release);
        g_seatOn.store(true, std::memory_order_release);
        spdlog::info("[substrate] anchor JOIN GUARD seat ON: at Actor::StartCombat's self check a NULL-target entry into a "
                     "substrate group by an actor that is not a live holder is refused; entry-route trace {}.",
                     g_trace ? "ON ([CombatSubstrate] bSubstrateEntryTrace=1)" : "off");
    }

    const char* SeatState() {
        const char* s = g_seatState.load(std::memory_order_acquire);
        return s ? s : "unknown";
    }

    bool AnswerJoin(RE::Actor* a_self, RE::Actor* a_target, RE::CombatGroup* a_group, std::uintptr_t a_siteRet) {
        if (!g_seatOn.load(std::memory_order_acquire) || !a_self) return false;
        const bool         live = subs::AnyLive();
        const std::uint8_t kind = GroupKind(a_group);
        if (!live && kind == 0) return false;
        const RE::FormID self   = a_self->GetFormID();
        const RE::FormID target = a_target ? a_target->GetFormID() : 0;
        const bool       holder = LiveHolder(self);
        const bool       refuse = !a_target && a_group && kind != 0 && !holder;
        const bool       trace  = live && g_trace;
        if (!refuse && !trace) return false;

        const auto base   = REL::Module::get().base();
        const auto caller = apmf::facetattr::CallerBelow(a_siteRet);
        const auto rva    = caller.found && caller.addr >= base ? caller.addr - base : caller.addr;
        const auto now    = apmf::clock::MonotonicMs();
        const auto grp    = reinterpret_cast<std::uintptr_t>(a_group);
        std::uint64_t n = 0;
        if (refuse) n = g_refusals.fetch_add(1, std::memory_order_relaxed) + 1;

        std::scoped_lock lk(g_logMx);
        if (trace) {
            if (now - g_traceWinStart >= kTraceWinMs) {
                if (g_traceSuppressed)
                    spdlog::info("[substrate] ENTRY trace: {} StartCombat change(s) not logged in the last {} s (cap {}).",
                                 g_traceSuppressed, kTraceWinMs / 1000, kTraceMax);
                g_traceWinStart   = now;
                g_traceLines      = 0;
                g_traceSuppressed = 0;
            }
            if (g_traceLast.size() > 512) g_traceLast.clear();
            const TraceKey key{ target, grp, rva };
            auto           it  = g_traceLast.find(self);
            if (it == g_traceLast.end() || !(it->second == key)) {
                g_traceLast[self] = key;
                if (g_traceLines < kTraceMax) {
                    ++g_traceLines;
                    spdlog::info("[substrate] ENTRY StartCombat 0x{}{} target(rsi)=0x{} group(rbp)=0x{} [{}] caller RVA "
                                 "0x{} ({}){}",
                                 Hex(self), holder ? " (a live holder)" : "", Hex(target), Hex(grp, 1), KindText(kind),
                                 Hex(rva, 1), caller.found ? "found" : "caller frame not found",
                                 refuse ? " -> REFUSED (null-target join into a substrate group)" : "");
                } else {
                    ++g_traceSuppressed;
                }
            }
        }
        if (refuse) {
            if (g_refuseLast.size() > 256) g_refuseLast.clear();
            const RE::FormID rk = self ^ static_cast<RE::FormID>(grp >> 4);
            auto&            last = g_refuseLast[rk];
            if (last == 0 || now - last >= kRefuseLogMs) {
                last = now;
                spdlog::warn("[substrate] 0x{} NULL-TARGET GROUP JOIN REFUSED: StartCombat(target none, group 0x{} [{}]) by "
                             "an actor that is not a live substrate holder, caller RVA 0x{}{}; refused at its own self "
                             "check, before it did anything ({} so far).",
                             Hex(self), Hex(grp, 1), KindText(kind), Hex(rva, 1),
                             caller.found ? "" : " (caller frame not found)", n);
            }
        }
        return refuse;
    }

    std::vector<RE::ActorHandle> GroupMembers(RE::Actor* a_holder) {
        auto* cc = a_holder ? a_holder->GetActorRuntimeData().combatController : nullptr;
        if (!cc || !cc->combatGroup) return {};
        return ReadGroup(cc->combatGroup, 0, true).members;
    }

    void AfterClose(RE::FormID a_holder, const std::vector<RE::ActorHandle>& a_members) {
        if (a_members.empty()) return;
        auto* anchor = RE::TESForm::LookupByID<RE::Actor>(subs::AnchorId());
        if (!anchor) return;
        const auto        now = apmf::clock::MonotonicMs();
        const std::string ctx = fmt::format("close of 0x{}", Hex(a_holder));
        for (const auto& mh : a_members) {
            const auto mp = mh.get();
            auto*      m  = mp.get();
            if (!m || m->GetFormID() == a_holder) continue;
            Treat(m, anchor, ctx, now, true);
        }
    }

    void Sweep(std::uint64_t a_now) {
        if (!subs::Installed()) return;
        const bool live = subs::AnyLive();
        if (live) g_tailUntil = a_now + kTailMs;
        if (!live && a_now >= g_tailUntil) {
            if (g_sweeping) {
                g_sweeping = false;
                for (std::size_t i = 0; i < kMaxGroups; ++i) g_groups[i].store(0, std::memory_order_release);
                if (g_leakTotal)
                    spdlog::error("[substrate] ANCHOR LEAK sweep window ends: {} leak line(s) this window ({} actor(s)).",
                                  g_leakTotal, g_leaks.size());
                g_leaks.clear();
                g_leakTotal = 0;
            }
            return;
        }
        g_sweeping = true;
        auto* anchor = RE::TESForm::LookupByID<RE::Actor>(subs::AnchorId());
        auto* pl     = RE::ProcessLists::GetSingleton();
        if (!anchor || !pl) return;
        const std::uint32_t anchorH = anchor->GetHandle().native_handle();

        std::array<std::uintptr_t, kMaxGroups> pub{};
        std::array<std::uint8_t, kMaxGroups>   kinds{};
        std::size_t                            npub = 0;
        auto publish = [&](RE::CombatGroup* g, std::uint8_t k) {
            const auto p = reinterpret_cast<std::uintptr_t>(g);
            for (std::size_t i = 0; i < npub; ++i)
                if (pub[i] == p) { kinds[i] |= k; return; }
            if (npub < kMaxGroups) { pub[npub] = p; kinds[npub] = k; ++npub; }
        };
        std::vector<RE::FormID> leaks;
        pl->ForEachHighActor([&](RE::Actor* a) {
            if (!a || a == anchor) return RE::BSContainer::ForEachResult::kContinue;
            auto* cc = a->GetActorRuntimeData().combatController;
            if (!cc) return RE::BSContainer::ForEachResult::kContinue;
            RE::CombatGroup*    g  = cc->combatGroup;
            const RE::FormID    id = a->GetFormID();
            const std::uint32_t st = subs::StateOf(id);
            if (st == APMF_API::kSubstrateState_Held || st == APMF_API::kSubstrateState_Real) {
                if (st == APMF_API::kSubstrateState_Held && g) {
                    const GroupRead gr = ReadGroup(g, anchorH, false);
                    if (gr.other == 0) publish(g, static_cast<std::uint8_t>(kKindHeld | (gr.listed ? kKindListed : 0)));
                }
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const bool      tAnchor = cc->targetHandle.native_handle() == anchorH;
            const GroupRead gr      = ReadGroup(g, anchorH, false);
            if (g && gr.listed && gr.other == 0) publish(g, kKindListed);
            if (tAnchor || gr.listed) leaks.push_back(id);
            return RE::BSContainer::ForEachResult::kContinue;
        });
        for (std::size_t i = 0; i < kMaxGroups; ++i) {
            g_kinds[i].store(i < npub ? kinds[i] : 0, std::memory_order_relaxed);
            g_groups[i].store(i < npub ? pub[i] : 0, std::memory_order_release);
        }
        for (const auto id : leaks)
            if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id)) Treat(a, anchor, "sweep", a_now, false);
    }

    void ResetAll() {
        for (std::size_t i = 0; i < kMaxGroups; ++i) g_groups[i].store(0, std::memory_order_release);
        g_leaks.clear();
        g_sweeping  = false;
        g_tailUntil = 0;
        g_leakTotal = 0;
        std::scoped_lock lk(g_logMx);
        g_traceLast.clear();
        g_refuseLast.clear();
    }

}
