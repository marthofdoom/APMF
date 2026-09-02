#include "PCH.h"
#include "core/Arbiter.h"
#include "core/Registry.h"

namespace apmf {

    namespace {
        constexpr std::uint64_t kObsEvery = 60;   // observability ~1/s @ 60fps

        const char* PkgTypeName(RE::TESPackage* pkg) {
            if (!pkg) return "<none>";
            const char* n = pkg->GetObjectTypeName();
            return n ? n : "<unnamed>";
        }
    }

    Arbiter& Arbiter::Get() {
        static Arbiter s_instance;
        return s_instance;
    }

    RE::Actor* Arbiter::EnsureTarget() {
        // While engaged, hold the current target -- do NOT let the crosshair
        // reassign control mid-session.
        if (Registry::Get().AnyEngaged()) {
            if (auto* kept = target.load(); kept && handle.get()) return kept;
        }

        RE::Actor* fromCrosshair = nullptr;
        if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
            if (auto ref = pick->targetActor.get()) fromCrosshair = ref->As<RE::Actor>();
        }
        if (fromCrosshair && !fromCrosshair->IsPlayerRef()) {
            if (fromCrosshair != target.load()) {
                auto* pkg     = fromCrosshair->GetCurrentPackage();
                pkgAtCapture  = pkg ? pkg->GetFormID() : 0;
                handle        = fromCrosshair->GetHandle();
                target.store(fromCrosshair);
                spdlog::info("[target] captured 0x{:08X} '{}' pkg=0x{:08X}({})",
                             fromCrosshair->GetFormID(),
                             fromCrosshair->GetName() ? fromCrosshair->GetName() : "?",
                             pkgAtCapture, PkgTypeName(pkg));
            }
            return fromCrosshair;
        }
        if (auto* kept = target.load(); kept && handle.get()) return kept;
        spdlog::warn("[target] REFUSED -- no actor under the crosshair. Aim directly at a follower/NPC.");
        return nullptr;
    }

    void Arbiter::ClearTargetIfIdle() {
        if (Registry::Get().AnyEngaged()) return;
        target.store(nullptr);
        handle       = RE::ActorHandle{};
        pkgAtCapture = 0;
    }

    void Arbiter::ReleaseAll(const char* why) {
        if (!Registry::Get().AnyEngaged()) return;
        auto* t = target.load();
        for (auto* ch : Registry::Get().All()) {
            if (ch->Engaged()) ch->Release(t);
        }
        spdlog::info("[release] all channels released ({})", why);
        ClearTargetIfIdle();
    }

    void Arbiter::DispatchHotkey(std::uint32_t code) {
        for (auto* ch : Registry::Get().All()) {
            for (const auto& hk : ch->Hotkeys()) {
                if (hk.code == code) {
                    ch->OnHotkey(code, EnsureTarget());
                    return;
                }
            }
        }
    }

    void Arbiter::OnActorUpdate(RE::Actor* actor) {
        if (!Registry::Get().AnyEngaged()) return;
        if (actor != target.load(std::memory_order_relaxed)) return;

        if (!handle.get()) { ReleaseAll("target-unloaded"); return; }

        const std::uint64_t tick = ticks.fetch_add(1, std::memory_order_relaxed);

        // Drive every engaged channel. Most channels no-op here (pure source-gate);
        // only a documented re-assert fallback does work per tick.
        for (auto* ch : Registry::Get().All()) {
            if (ch->Engaged()) ch->Tick(actor);
        }

        // Observability ~1/s: package coherence + which channels hold authority.
        if ((tick % kObsEvery) != 0) return;
        auto*      pkg  = actor->GetCurrentPackage();
        RE::FormID id   = pkg ? pkg->GetFormID() : 0;
        const bool same = (id == pkgAtCapture);
        std::string engaged;
        for (auto* ch : Registry::Get().All()) {
            if (ch->Engaged()) {
                if (!engaged.empty()) engaged += ',';
                engaged += ch->Name();
            }
        }
        spdlog::info("[obs] tgt=0x{:08X} pkg=0x{:08X}({}) [{}] engaged=[{}]",
                     actor->GetFormID(), id, PkgTypeName(pkg),
                     same ? "PACKAGE STABLE" : "PACKAGE CHANGED!!",
                     engaged.empty() ? "-" : engaged.c_str());
    }

}
