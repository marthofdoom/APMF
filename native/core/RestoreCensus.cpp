#include "PCH.h"
#include "core/Log.h"
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/RestoreCensus.h"

#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Win32 INI read, declared by hand exactly like core/CastSeats.cpp does (PCH does
// not pull in <Windows.h>).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// See core/RestoreCensus.h for the design. PASSIVE: this TU never writes to an
// actor, a caster or the engine. Two threads touch it:
//   * the COMBAT thread(s), through NoteRestoreSeat (core/CastSeats.cpp), which
//     only counts into an already-open window under g_mx; and
//   * the GAME thread, through Poll (Arbiter::OncePerFrame), which reads the
//     engine FIRST with no lock held, then reconciles windows under g_mx, then
//     logs after releasing it. g_mx is a leaf lock, never held across an engine
//     call or a log call.
// ============================================================================

namespace apmf::restorecensus {

    namespace {

        constexpr std::uint64_t kPollIntervalMs  = 200;
        constexpr std::uint64_t kHeartbeatMs     = 60000;
        constexpr std::uint64_t kBudgetWindowMs  = 10000;
        constexpr std::uint32_t kBudgetLines     = 30;
        constexpr std::array<std::uint64_t, 5> kZeroMilestonesMs{ { 2000, 5000, 10000, 20000, 40000 } };
        constexpr std::size_t   kMaxCasters      = 8;

        constexpr std::uint32_t kSeatStart  = 0x06;
        constexpr std::uint32_t kSeatTarget = 0x0A;

        std::atomic<bool>        g_armed{ false };
        std::atomic<std::size_t> g_openCount{ 0 };   // Active()'s pre-gate; written under g_mx

        struct Window {
            // identity
            RE::FormID actor  = 0;
            char       hand   = '?';   // 'L', 'R', 'D' (dual-cast claim on both)
            RE::FormID driven = 0;
            // claim facts at open (target updated if the claim is repointed)
            RE::FormID spell  = 0;
            RE::FormID proxy  = 0;
            RE::FormID target = 0;
            bool       self   = true;
            bool       conc   = false;
            std::uint32_t targetChanges = 0;
            // timing
            std::uint64_t openMs          = 0;
            std::uint64_t firstSeatMs     = 0;   // first Restore seat call on the driven form
            std::uint64_t firstClaimYesMs = 0;   // first 0x06 answered YES by the claim
            std::uint64_t firstEngageMs   = 0;   // first poll with the claimed hand holding the driven form
            // counts (combat thread writes under g_mx)
            std::uint32_t seat06        = 0;
            std::uint32_t seat06Claim   = 0;
            std::uint32_t seat06NativeY = 0;
            std::uint32_t seat0A        = 0;
            std::uint32_t otherItemObs  = 0;
            RE::FormID    firstOther    = 0;
            std::array<const void*, kMaxCasters> casters{};
            std::uint32_t nCasters      = 0;
            // game thread only
            std::uint32_t maxState      = 0;
            std::uint32_t polls         = 0;
            std::uint32_t pollsNoCtrl   = 0;
            std::size_t   nextMilestone = 0;
            bool          seenThisPoll  = false;
        };

        std::mutex                                        g_mx;
        std::unordered_map<RE::FormID, std::vector<Window>> g_windows;   // by actor, at most 2 (one per hand)

        // game-thread-only state
        std::uint64_t g_lastPollMs      = 0;
        std::uint64_t g_lastHeartbeatMs = 0;
        std::uint64_t g_budgetStartMs   = 0;
        std::uint32_t g_budgetUsed      = 0;
        std::uint32_t g_budgetDropped   = 0;
        bool          g_iniRead         = false;
        std::uint32_t g_opened = 0, g_closed = 0, g_zeroClosed = 0, g_engagedClosed = 0;
        std::unordered_map<RE::FormID, bool> g_shapeCache;   // load-order driven form -> restore-shaped

        const char* HandName(char h) {
            return h == 'L' ? "left" : h == 'R' ? "right" : h == 'D' ? "both (dual)" : "?";
        }

        // A restore-shaped form: its costliest effect is a non-hostile, non-detrimental
        // Value / PeakValue / DualValue modifier on Health, Magicka or Stamina -- the
        // (archetype, AV, non-hostile) shape the engine's classifier keys into the
        // Restore row for (core/CastClassify.h). A claim on anything else is served by
        // another caster category and is not this census's question. Game thread.
        bool RestoreShaped(RE::FormID a_form) {
            if (const auto it = g_shapeCache.find(a_form); it != g_shapeCache.end()) return it->second;
            bool shaped = false;
            if (auto* item = RE::TESForm::LookupByID<RE::MagicItem>(a_form)) {
                if (auto* eff = item->GetCostliestEffectItem(); eff && eff->baseEffect) {
                    const auto* mgef = eff->baseEffect;
                    const auto  arch = mgef->GetArchetype();
                    const auto  av   = mgef->data.primaryAV;
                    shaped = !mgef->IsHostile() && !mgef->IsDetrimental() &&
                             (arch == RE::EffectArchetypes::ArchetypeID::kValueModifier ||
                              arch == RE::EffectArchetypes::ArchetypeID::kPeakValueModifier ||
                              arch == RE::EffectArchetypes::ArchetypeID::kDualValueModifier) &&
                             (av == RE::ActorValue::kHealth || av == RE::ActorValue::kMagicka ||
                              av == RE::ActorValue::kStamina);
                }
            }
            // Never cache a runtime-minted (0xFF) form: delivery-flip proxies are pooled, so an id
            // can name a different spell after a release. Load-order forms are stable.
            if ((a_form >> 24) != 0xFF) g_shapeCache.emplace(a_form, shaped);
            return shaped;
        }

        // One global line budget. Game thread only (every census line is printed from Poll).
        bool Budget(std::uint64_t a_now) {
            if (a_now - g_budgetStartMs >= kBudgetWindowMs) {
                if (g_budgetDropped)
                    spdlog::info("[census] {} line(s) suppressed by the budget ({} per {} ms)", g_budgetDropped,
                                 kBudgetLines, kBudgetWindowMs);
                g_budgetStartMs = a_now;
                g_budgetUsed    = 0;
                g_budgetDropped = 0;
            }
            if (g_budgetUsed >= kBudgetLines) {
                ++g_budgetDropped;
                return false;
            }
            ++g_budgetUsed;
            return true;
        }

        std::string Ms(std::uint64_t a_from, std::uint64_t a_at) {
            return a_at ? std::to_string(a_at - a_from) + " ms" : std::string("never");
        }

        std::string CloseLine(const Window& w, std::uint64_t a_now, const char* a_why) {
            const bool        zero    = (w.seat06 + w.seat0A) == 0;
            const bool        engaged = w.firstEngageMs != 0;
            std::string       verdict;
            if (engaged)
                verdict = "ENGAGED (the claimed hand took the driven form)";
            else if (!zero)
                verdict = "RESTORE DELIBERATED, the hand never took the driven form";
            else
                verdict = "ZERO (no Restore caster ever reached a seat for the driven form)";
            return fmt::format(
                "[census] 0x{} CLOSE hand={} driven=0x{} target={} 0x{} after {} ms ({}): {} | "
                "restore casters seen={} seat06={} (claim YES={}, native YES={}) seat0A={} | "
                "first seat {} / first claim YES {} / first hand engage {} | max caster state={} | "
                "other restore items={} (first 0x{}) | no combat controller {}/{} polls | target changes={}",
                apmf::log::Hex(w.actor), HandName(w.hand), apmf::log::Hex(w.driven), w.self ? "self" : "ally",
                apmf::log::Hex(w.target), a_now - w.openMs, a_why, verdict, w.nCasters, w.seat06, w.seat06Claim,
                w.seat06NativeY, w.seat0A, Ms(w.openMs, w.firstSeatMs), Ms(w.openMs, w.firstClaimYesMs),
                Ms(w.openMs, w.firstEngageMs), w.maxState, w.otherItemObs, apmf::log::Hex(w.firstOther),
                w.pollsNoCtrl, w.polls, w.targetChanges);
        }

        // What Poll learned about one live claim, gathered with no lock held.
        struct Seen {
            char                hand = '?';
            apmf::CastSeatClaim claim{};
            RE::FormID          driven   = 0;
            std::uint32_t       state    = 0;      // claimed hand caster state (max over both for 'D')
            bool                holds    = false;  // that caster's currentSpell == driven, state != 0
        };

        std::uint32_t HandState(RE::Actor* a_actor, RE::MagicSystem::CastingSource a_src, RE::FormID a_driven,
                                bool& a_holds) {
            auto* mc = a_actor->GetMagicCaster(a_src);
            if (!mc) return 0;
            const auto st  = static_cast<std::uint32_t>(mc->state.get());
            const auto sid = mc->currentSpell ? mc->currentSpell->GetFormID() : 0;
            if (st != 0 && sid == a_driven) {
                a_holds = true;
                return st;
            }
            return 0;
        }

    }

    bool Active() {
        return g_armed.load(std::memory_order_relaxed) && g_openCount.load(std::memory_order_relaxed) != 0;
    }

    void NoteRestoreSeat(RE::FormID a_actor, const void* a_caster, RE::FormID a_item, std::uint32_t a_seat,
                         bool a_claimAnswered, bool a_answer) {
        if (!Active() || a_actor == 0) return;
        const auto now = apmf::clock::MonotonicMs();
        std::scoped_lock lk(g_mx);
        const auto it = g_windows.find(a_actor);
        if (it == g_windows.end()) return;
        for (auto& w : it->second) {
            if (a_item != w.driven) {
                if (w.otherItemObs++ == 0) w.firstOther = a_item;
                continue;
            }
            if (!w.firstSeatMs) w.firstSeatMs = now;
            if (a_seat == kSeatStart) {
                ++w.seat06;
                if (a_claimAnswered) {
                    ++w.seat06Claim;
                    if (a_answer && !w.firstClaimYesMs) w.firstClaimYesMs = now;
                } else if (a_answer) {
                    ++w.seat06NativeY;
                }
            } else if (a_seat == kSeatTarget) {
                ++w.seat0A;
            }
            bool known = false;
            for (std::uint32_t i = 0; i < w.nCasters; ++i) known |= (w.casters[i] == a_caster);
            if (!known && w.nCasters < kMaxCasters) w.casters[w.nCasters++] = a_caster;
        }
    }

    void Poll() {
        if (!g_iniRead) {
            g_iniRead = true;
            const bool on = GetPrivateProfileIntA("Probe", "bRestoreCensus", 1, "Data/SKSE/Plugins/APMF.ini") != 0;
            g_armed.store(on, std::memory_order_relaxed);
            spdlog::info("[census] Restore-caster census {} ([Probe] bRestoreCensus={}). PASSIVE: per restore-shaped "
                         "kIntent_Cast claim it counts the Restore seats the engine reaches (0x06/0x0A), the claimed "
                         "hand taking the form, and the ZERO case. Field-test default ON; reset before release.",
                         on ? "ARMED" : "NOT armed", on ? 1 : 0);
        }
        if (!g_armed.load(std::memory_order_relaxed)) return;

        const auto now = apmf::clock::MonotonicMs();
        if (now - g_lastPollMs < kPollIntervalMs) return;
        g_lastPollMs = now;
        if (!g_budgetStartMs) g_budgetStartMs = now;

        // ---- 1. read the engine and the published claims, NO lock held ----
        struct ActorSeen {
            RE::FormID        fid  = 0;
            bool              ctrl = false;
            std::vector<Seen> seen;
        };
        std::vector<ActorSeen> actors;
        if (auto* pl = RE::ProcessLists::GetSingleton()) {
            for (auto& handle : pl->highActorHandles) {
                auto a = handle.get();
                auto* actor = a.get();
                if (!actor || actor->IsPlayerRef()) continue;
                const RE::FormID fid = actor->GetFormID();

                apmf::CastSeatClaim cl{}, cr{};
                const bool hasL = apmf::ControlMap::Get().TryGetCastSeatClaimForHand(fid, apmf::CastHand::kLeft, cl);
                const bool hasR = apmf::ControlMap::Get().TryGetCastSeatClaimForHand(fid, apmf::CastHand::kRight, cr);
                if (!hasL && !hasR) continue;

                ActorSeen as{};
                as.fid  = fid;
                as.ctrl = actor->GetActorRuntimeData().combatController != nullptr;
                auto add = [&](char h, const apmf::CastSeatClaim& c) {
                    if (c.flags & APMF_API::kCastFlag_DenyHandOnly) return;     // a floor drives nothing
                    const RE::FormID driven = c.proxy ? c.proxy : c.spell;
                    if (!driven || !RestoreShaped(driven)) return;
                    Seen s{};
                    s.hand   = h;
                    s.claim  = c;
                    s.driven = driven;
                    if (h == 'L' || h == 'D')
                        s.state = (std::max)(s.state, HandState(actor, RE::MagicSystem::CastingSource::kLeftHand, driven, s.holds));
                    if (h == 'R' || h == 'D')
                        s.state = (std::max)(s.state, HandState(actor, RE::MagicSystem::CastingSource::kRightHand, driven, s.holds));
                    as.seen.push_back(s);
                };
                const RE::FormID dl = hasL ? (cl.proxy ? cl.proxy : cl.spell) : 0;
                const RE::FormID dr = hasR ? (cr.proxy ? cr.proxy : cr.spell) : 0;
                const bool dual = hasL && hasR && (cl.flags & APMF_API::kCastFlag_DualCast) && dl == dr;
                if (dual) {
                    add('D', cl);
                } else {
                    if (hasL) add('L', cl);
                    if (hasR) add('R', cr);
                }
                if (!as.seen.empty()) actors.push_back(std::move(as));
            }
        }

        // ---- 2. reconcile windows under the leaf lock; collect lines ----
        std::vector<std::string> lines;
        {
            std::scoped_lock lk(g_mx);
            for (auto& [fid, ws] : g_windows)
                for (auto& w : ws) w.seenThisPoll = false;

            for (const auto& as : actors) {
                auto& ws = g_windows[as.fid];
                for (const auto& s : as.seen) {
                    Window* w = nullptr;
                    for (auto& x : ws)
                        if (x.hand == s.hand && x.driven == s.driven) w = &x;
                    if (!w) {
                        Window n{};
                        n.actor  = as.fid;
                        n.hand   = s.hand;
                        n.driven = s.driven;
                        n.spell  = s.claim.spell;
                        n.proxy  = s.claim.proxy;
                        n.target = s.claim.target;
                        n.self   = (s.claim.target == 0 || s.claim.target == as.fid);
                        n.conc   = (s.claim.flags & APMF_API::kCastFlag_Concentration) != 0;
                        n.openMs = now;
                        ws.push_back(n);
                        w = &ws.back();
                        ++g_opened;
                        lines.push_back(fmt::format(
                            "[census] 0x{} OPEN hand={} spell=0x{} driven=0x{}{} target={} 0x{} conc={} "
                            "combat controller={} -- counting Restore seats for the driven form from now",
                            apmf::log::Hex(as.fid), HandName(n.hand), apmf::log::Hex(n.spell), apmf::log::Hex(n.driven),
                            n.proxy ? " (delivery-flip proxy)" : "", n.self ? "self" : "ally", apmf::log::Hex(n.target),
                            n.conc ? 1 : 0, as.ctrl ? "yes" : "NO"));
                    }
                    w->seenThisPoll = true;
                    ++w->polls;
                    if (!as.ctrl) ++w->pollsNoCtrl;
                    if (s.claim.target != w->target) {
                        w->target = s.claim.target;
                        w->self   = (s.claim.target == 0 || s.claim.target == as.fid);
                        ++w->targetChanges;
                    }
                    if (s.holds) {
                        if (s.state > w->maxState) w->maxState = s.state;
                        if (!w->firstEngageMs) {
                            w->firstEngageMs = now;
                            lines.push_back(fmt::format(
                                "[census] 0x{} ENGAGE hand={} driven=0x{} target={} caster state={} at +{} ms "
                                "(first Restore seat {}, first claim YES {})",
                                apmf::log::Hex(w->actor), HandName(w->hand), apmf::log::Hex(w->driven),
                                w->self ? "self" : "ally", s.state, now - w->openMs, Ms(w->openMs, w->firstSeatMs),
                                Ms(w->openMs, w->firstClaimYesMs)));
                        }
                    }
                    const bool zero = (w->seat06 + w->seat0A) == 0;
                    while (w->nextMilestone < kZeroMilestonesMs.size() &&
                           now - w->openMs >= kZeroMilestonesMs[w->nextMilestone]) {
                        if (zero)
                            lines.push_back(fmt::format(
                                "[census] 0x{} STILL ZERO hand={} driven=0x{} target={} after {} ms: no Restore caster "
                                "has reached a seat for the driven form (combat controller {}, other restore items={})",
                                apmf::log::Hex(w->actor), HandName(w->hand), apmf::log::Hex(w->driven),
                                w->self ? "self" : "ally", now - w->openMs, as.ctrl ? "yes" : "NO", w->otherItemObs));
                        ++w->nextMilestone;
                    }
                }
            }

            // Close every window this poll did not see: claim released/expired/re-pointed
            // to another form, or the actor left the high process list.
            for (auto it = g_windows.begin(); it != g_windows.end();) {
                auto& ws = it->second;
                for (auto wi = ws.begin(); wi != ws.end();) {
                    if (wi->seenThisPoll) { ++wi; continue; }
                    const bool zero = (wi->seat06 + wi->seat0A) == 0;
                    ++g_closed;
                    if (wi->firstEngageMs) ++g_engagedClosed;
                    else if (zero) ++g_zeroClosed;
                    lines.push_back(CloseLine(*wi, now, "claim gone, re-pointed, or actor left high process"));
                    wi = ws.erase(wi);
                }
                if (ws.empty()) it = g_windows.erase(it);
                else ++it;
            }

            std::size_t open = 0;
            for (const auto& [fid, ws] : g_windows) open += ws.size();
            g_openCount.store(open, std::memory_order_relaxed);

            if (now - g_lastHeartbeatMs >= kHeartbeatMs) {
                g_lastHeartbeatMs = now;
                lines.push_back(fmt::format(
                    "[census] HEARTBEAT windows open={} | since start: opened={} closed={} (engaged={}, zero={}, "
                    "deliberated-no-engage={})",
                    open, g_opened, g_closed, g_engagedClosed, g_zeroClosed,
                    g_closed - g_engagedClosed - g_zeroClosed));
            }
        }

        // ---- 3. log with no lock held ----
        for (const auto& l : lines) {
            const bool heartbeat = l.rfind("[census] HEARTBEAT", 0) == 0;
            if (heartbeat || Budget(now)) spdlog::info("{}", l);
        }
    }

}
