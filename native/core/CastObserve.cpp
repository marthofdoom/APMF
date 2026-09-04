#include "PCH.h"
#include "core/Log.h"
#include "core/CastObserve.h"
#include "core/Clock.h"

#include <array>
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

        // Read one hand's caster; log on a state/spell transition. Returns whether the
        // actor appears to be actively casting (so the poll can register the sink).
        bool ObserveHand(RE::Actor* a_actor, RE::MagicSystem::CastingSource a_src,
                         std::size_t a_srcIdx, const char* a_srcName) {
            auto* mc = a_actor->GetMagicCaster(a_src);
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

    }

    void Poll() {
        // GAME THREAD (Arbiter::OncePerFrame == the PlayerCharacter/Drain seat, main
        // thread). Self-throttle so this is near-free.
        const auto now = apmf::clock::MonotonicMs();
        if (now - g_lastPollMs < kPollIntervalMs) return;
        g_lastPollMs = now;

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

}
