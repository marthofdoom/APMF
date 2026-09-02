#include "PCH.h"

// ============================================================================
// APMF — AI Package Management Framework. PROTOTYPE 1.
//
// Prototype 0 proved the make-or-break (design.md §5): the central
// Actor::Update(0xAD) hook + a movement-layer commandeer keeps the target's
// PACKAGE fully coherent (every [obs] read PACKAGE STABLE, no CTD). But the
// prototype-0 executor (rewriting Actor::Move's delta) only SCALED the engine's
// own movement -> a "bird in the wind" slide with no locomotion animation.
//
// Prototype 1:
//   1. FIX movement with a PROPER animated drive. Research (see report) showed
//      the IMovementDirectControl feed is un-named in every CommonLib fork
//      (blind-vtable CTD risk) and that SetControlsDriven SUSPENDS the AI
//      planner (the opposite of "leave the package running"). The version-robust
//      route is KeepOffsetFromActor, bound through Address Library
//      (RELOCATION_ID(SE,AE) -> resolves per runtime, NO hardcoded call-site
//      offset). It is the vanilla follow mechanism: real navmesh path + anim
//      graph + physics, layered OVER the running package (package keeps
//      evaluating). Anchor must be an Actor, so we anchor to the player: the
//      target WALKS to the player. On yield we ClearKeepOffsetFromActor and the
//      package's own locomotion resumes.
//   2. Tier-A facet: LOOK STRAIGHT UP (headtrack a point above the head),
//      re-asserted every tick inside 0xAD so the AI cannot repoint the head.
//   3. Tier-A facet: CROUCH (SneakStart), re-asserted every tick so the AI
//      cannot stand the follower back up.
//
// All three are gated to ONE crosshair-picked target, held UNDER APMF AUTHORITY
// (re-asserted each tick in the central hook), package NEVER substituted, and
// released cleanly. Version-robust: vtable-index hooks + named CommonLib methods
// + Address-Library relocations only. No hardcoded call-site offsets.
// ============================================================================

namespace {

    // ---- Address-Library-bound engine functions ----------------------------
    // RELOCATION_ID(SE_id, AE_id) resolves through Address Library per runtime
    // (correct on 1.6.1170 and every other AE/SE build) -- this is NOT a
    // hardcoded call-site offset. IDs cross-checked against multiple open RE
    // sources (Adventurers-Like-You Util.h; activeragdoll offsets.cpp).
    namespace Native {
        void KeepOffsetFromActor(RE::Actor* a_actor, const RE::ActorHandle& a_target,
                                 const RE::NiPoint3& a_posOffset, const RE::NiPoint3& a_angleOffset,
                                 float a_catchUpRadius, float a_followRadius) {
            using func_t = void (*)(RE::Actor*, const RE::ActorHandle&, const RE::NiPoint3&,
                                    const RE::NiPoint3&, float, float);
            REL::Relocation<func_t> func{ RELOCATION_ID(36870, 37894) };
            func(a_actor, a_target, a_posOffset, a_angleOffset, a_catchUpRadius, a_followRadius);
        }
        void ClearKeepOffsetFromActor(RE::Actor* a_actor) {
            using func_t = void (*)(RE::Actor*);
            REL::Relocation<func_t> func{ RELOCATION_ID(36871, 37895) };
            func(a_actor);
        }
        void SetDontMove(RE::Actor* a_actor, bool a_dontMove) {
            using func_t = void (*)(RE::Actor*, bool);
            REL::Relocation<func_t> func{ RELOCATION_ID(36490, 37489) };
            func(a_actor, a_dontMove);
        }
    }

    // ---- Log ---------------------------------------------------------------
    void SetupLog() {
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;
        try {
            sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("Data/SKSE/Plugins/APMF.log", true);
        } catch (const spdlog::spdlog_ex&) {
            if (auto dir = SKSE::log::log_directory()) {
                try {
                    sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "APMF.log").string(), true);
                } catch (const spdlog::spdlog_ex&) { return; }
            } else { return; }
        }
        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);   // prototype: flush every line so a CTD keeps the trail
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    }

    // ---- Controls (DirectInput scancodes) ----------------------------------
    constexpr std::uint32_t kKeyMoveEngage    = 0x4F;   // DIK_NUMPAD1 : movement ENGAGE (walk to player)
    constexpr std::uint32_t kKeyMoveDisengage = 0x51;   // DIK_NUMPAD3 : movement DISENGAGE (yield)
    constexpr std::uint32_t kKeyLookUpToggle  = 0x4B;   // DIK_NUMPAD4 : look-straight-up TOGGLE
    constexpr std::uint32_t kKeyCrouchToggle  = 0x47;   // DIK_NUMPAD7 : crouch TOGGLE

    // KeepOffsetFromActor tuning: come right up to the player and hold.
    constexpr float kCatchUpRadius = 100.0f;
    constexpr float kFollowRadius  = 50.0f;
    // How high above the head to plant the headtrack point (world units).
    constexpr float kLookUpHeight  = 300.0f;
    // Re-issue KeepOffsetFromActor this often (ticks) as authority insurance.
    constexpr std::uint64_t kMoveReassertEvery = 180;   // ~3s @ 60fps
    constexpr std::uint64_t kObsEvery          = 60;    // ~1s @ 60fps

    // ---- Gated state (main/sim thread only) --------------------------------
    std::atomic<bool>          g_moveOn{ false };
    std::atomic<bool>          g_lookUpOn{ false };
    std::atomic<bool>          g_crouchOn{ false };

    std::atomic<RE::Actor*>    g_targetPtr{ nullptr };     // fast identity compare in the hot thunk
    RE::ActorHandle            g_targetHandle{};           // liveness source of truth
    RE::ActorHandle            g_playerHandle{};           // movement anchor
    RE::FormID                 g_pkgAtCapture{ 0 };        // package the target held when captured
    std::atomic<std::uint64_t> g_ticks{ 0 };

    bool AnyFacetOn() {
        return g_moveOn.load(std::memory_order_relaxed) ||
               g_lookUpOn.load(std::memory_order_relaxed) ||
               g_crouchOn.load(std::memory_order_relaxed);
    }

    const char* PkgTypeName(RE::TESPackage* a_pkg) {
        if (!a_pkg) return "<none>";
        const char* n = a_pkg->GetObjectTypeName();
        return n ? n : "<unnamed>";
    }

    // Resolve the target: prefer the crosshair actor (so a tester just aims);
    // fall back to the already-captured target. (Re)captures package identity.
    RE::Actor* EnsureTarget() {
        RE::Actor* fromCrosshair = nullptr;
        if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
            if (auto ref = pick->targetActor.get()) fromCrosshair = ref->As<RE::Actor>();
        }
        if (fromCrosshair && !fromCrosshair->IsPlayerRef()) {
            if (fromCrosshair != g_targetPtr.load()) {
                auto* pkg = fromCrosshair->GetCurrentPackage();
                g_pkgAtCapture = pkg ? pkg->GetFormID() : 0;
                g_targetHandle = fromCrosshair->GetHandle();
                g_targetPtr.store(fromCrosshair);
                spdlog::info("[target] captured 0x{:08X} '{}' pkg=0x{:08X}({})",
                             fromCrosshair->GetFormID(),
                             fromCrosshair->GetName() ? fromCrosshair->GetName() : "?",
                             g_pkgAtCapture, PkgTypeName(pkg));
            }
            return fromCrosshair;
        }
        if (auto* kept = g_targetPtr.load(); kept && g_targetHandle.get()) return kept;
        spdlog::warn("[target] REFUSED -- no actor under the crosshair. Aim directly at a follower/NPC.");
        return nullptr;
    }

    void ClearTargetIfIdle() {
        if (AnyFacetOn()) return;
        g_targetPtr.store(nullptr);
        g_targetHandle = RE::ActorHandle{};
        g_pkgAtCapture = 0;
    }

    // ---- Facet: MOVEMENT (KeepOffsetFromActor -> walk to the player) --------
    void MoveEngage() {
        auto* target = EnsureTarget();
        if (!target) return;
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) return;
        g_playerHandle = pc->GetHandle();
        Native::SetDontMove(target, false);
        Native::KeepOffsetFromActor(target, g_playerHandle, RE::NiPoint3{ 0.0f, 0.0f, 0.0f },
                                    RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, kCatchUpRadius, kFollowRadius);
        g_moveOn.store(true);
        spdlog::info("[move] ENGAGE tgt=0x{:08X} -> KeepOffsetFromActor(player) asserted. Should WALK "
                     "(navmesh+anim+physics) to the player; package left untouched. dist={:.1f}",
                     target->GetFormID(), target->GetPosition().GetDistance(pc->GetPosition()));
    }

    void MoveDisengage() {
        auto* target = g_targetPtr.load();
        if (g_moveOn.exchange(false) && target) {
            Native::ClearKeepOffsetFromActor(target);
            auto* pkg = target->GetCurrentPackage();
            spdlog::info("[yield] MOVE DISENGAGE tgt=0x{:08X} -> ClearKeepOffsetFromActor. Package "
                         "resumes its OWN locomotion. pkg-now=0x{:08X}({}) pkg-at-capture=0x{:08X}",
                         target->GetFormID(), pkg ? pkg->GetFormID() : 0, PkgTypeName(pkg), g_pkgAtCapture);
        }
        ClearTargetIfIdle();
    }

    // ---- Facet: LOOK STRAIGHT UP (headtrack a point above the head) ---------
    void LookUpToggle() {
        if (g_lookUpOn.load()) {
            g_lookUpOn.store(false);
            spdlog::info("[look] DISENGAGE -- stop asserting headtrack; AI resumes pointing the head.");
            ClearTargetIfIdle();
            return;
        }
        if (!EnsureTarget()) return;
        g_lookUpOn.store(true);
        spdlog::info("[look] ENGAGE -- craning head straight up; will RE-ASSERT every tick to hold "
                     "authority against the AI.");
    }

    // Assert once per tick (called from the 0xAD hook). Returns whether AI had
    // repointed the head (i.e. we had to win it back).
    bool AssertLookUp(RE::Actor* a_actor) {
        auto* proc = a_actor->GetActorRuntimeData().currentProcess;
        if (!proc) return false;
        const bool aiRepointed = static_cast<bool>(proc->GetHeadtrackTarget());   // AI set a refr target
        RE::NiPoint3 up = a_actor->GetPosition();
        up.z += kLookUpHeight;                       // directly overhead -> look straight up
        proc->SetHeadtrackTarget(a_actor, up);
        return aiRepointed;
    }

    // ---- Facet: CROUCH (SneakStart, held under authority) -------------------
    void CrouchToggle() {
        if (g_crouchOn.load()) {
            g_crouchOn.store(false);
            if (auto* t = g_targetPtr.load()) t->NotifyAnimationGraph("SneakStop"sv);
            spdlog::info("[crouch] DISENGAGE -- SneakStop; AI resumes its own stance.");
            ClearTargetIfIdle();
            return;
        }
        auto* target = EnsureTarget();
        if (!target) return;
        target->NotifyAnimationGraph("SneakStart"sv);
        g_crouchOn.store(true);
        spdlog::info("[crouch] ENGAGE -- SneakStart; will RE-ASSERT if the AI stands her up.");
    }

    // Assert once per tick. Returns whether the AI had stood the actor up (we
    // detected sneaking==false and re-asserted).
    bool AssertCrouch(RE::Actor* a_actor) {
        if (a_actor->IsSneaking()) return false;
        a_actor->NotifyAnimationGraph("SneakStart"sv);   // AI stood her up -> win it back
        return true;
    }

    void ReleaseAllFacets(const char* a_why) {
        if (!AnyFacetOn()) return;
        auto* t = g_targetPtr.load();
        if (g_moveOn.exchange(false) && t) Native::ClearKeepOffsetFromActor(t);
        if (g_crouchOn.exchange(false) && t) t->NotifyAnimationGraph("SneakStop"sv);
        g_lookUpOn.store(false);
        spdlog::info("[release] all facets released ({})", a_why);
        ClearTargetIfIdle();
    }

    // ---- Input sink --------------------------------------------------------
    class InputSink : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static InputSink* GetSingleton() { static InputSink s; return &s; }
        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
                                              RE::BSTEventSource<RE::InputEvent*>*) override {
            if (!a_event) return RE::BSEventNotifyControl::kContinue;
            for (auto* e = *a_event; e; e = e->next) {
                if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
                auto* btn = e->AsButtonEvent();
                if (!btn || !btn->IsDown()) continue;
                if (btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;
                switch (btn->GetIDCode()) {
                case kKeyMoveEngage:    MoveEngage();    break;
                case kKeyMoveDisengage: MoveDisengage(); break;
                case kKeyLookUpToggle:  LookUpToggle();  break;
                case kKeyCrouchToggle:  CrouchToggle();  break;
                default: break;
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ---- The central hook: Actor::Update @ 0xAD ----------------------------
    // Patched ONCE on the Character vtable -> every NPC routes through here. For
    // the single gated target we RE-ASSERT the held facets EVERY tick (this is
    // what "under APMF authority" means) and log observability. All other actors
    // just call the original.
    struct CharacterUpdateHook {
        static void thunk(RE::Actor* a_this, float a_delta) {
            func(a_this, a_delta);   // original FIRST -- we assert on top of the real AI tick
            if (!AnyFacetOn()) return;
            if (a_this != g_targetPtr.load(std::memory_order_relaxed)) return;

            if (!g_targetHandle.get()) { ReleaseAllFacets("target-unloaded"); return; }

            const std::uint64_t tick = g_ticks.fetch_add(1, std::memory_order_relaxed);

            // --- authority: re-assert every tick ---
            bool headFought = false, crouchFought = false;
            if (g_lookUpOn.load(std::memory_order_relaxed)) headFought   = AssertLookUp(a_this);
            if (g_crouchOn.load(std::memory_order_relaxed)) crouchFought = AssertCrouch(a_this);
            if (g_moveOn.load(std::memory_order_relaxed) && (tick % kMoveReassertEvery) == 0) {
                if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                    g_playerHandle = pc->GetHandle();
                    Native::KeepOffsetFromActor(a_this, g_playerHandle, RE::NiPoint3{ 0.0f, 0.0f, 0.0f },
                                                RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, kCatchUpRadius, kFollowRadius);
                }
            }

            // --- report a fight the same tick it happens ---
            if (headFought)
                spdlog::info("[look] AI repointed the head -> APMF re-asserted straight-up (authority held)");
            if (crouchFought)
                spdlog::info("[crouch] AI stood her up -> APMF re-asserted SneakStart (authority held)");

            // --- observability, ~1/s ---
            if ((tick % kObsEvery) != 0) return;
            auto*      pkg  = a_this->GetCurrentPackage();
            RE::FormID id   = pkg ? pkg->GetFormID() : 0;
            const bool same = (id == g_pkgAtCapture);
            float dist = 0.0f;
            if (auto* pc = RE::PlayerCharacter::GetSingleton()) dist = a_this->GetPosition().GetDistance(pc->GetPosition());
            auto* proc = a_this->GetActorRuntimeData().currentProcess;
            const bool aiHead = proc && static_cast<bool>(proc->GetHeadtrackTarget());
            spdlog::info("[obs] tgt=0x{:08X} pkg=0x{:08X}({}) [{}] facets[move={} lookUp={}({}) crouch={}({})] "
                         "dist-to-player={:.1f}",
                         a_this->GetFormID(), id, PkgTypeName(pkg),
                         same ? "PACKAGE STABLE" : "PACKAGE CHANGED!!",
                         g_moveOn.load() ? 1 : 0,
                         g_lookUpOn.load() ? 1 : 0, aiHead ? "ai-refr-set" : "ours",
                         g_crouchOn.load() ? 1 : 0, a_this->IsSneaking() ? "sneaking" : "STOOD",
                         dist);
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr std::size_t idx = 0x0AD;   // Actor::Update(float)
    };

    struct PlayerUpdateHook {
        static void thunk(RE::PlayerCharacter* a_this, float a_delta) {
            func(a_this, a_delta);
            static bool s_first = true;
            if (s_first) { s_first = false; spdlog::info("[hook] player Update seat live (0xAD firing)"); }
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr std::size_t idx = 0x0AD;
    };

    std::atomic<bool> g_hooksInstalled{ false };

    void InstallHooks() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[hook] VR runtime -- 0xAD index unverified for VR; hooks NOT installed.");
            return;
        }
        if (g_hooksInstalled.exchange(true)) return;

        REL::Relocation<std::uintptr_t> charVtbl{ RE::VTABLE_Character[0] };
        CharacterUpdateHook::func = charVtbl.write_vfunc(CharacterUpdateHook::idx, CharacterUpdateHook::thunk);

        REL::Relocation<std::uintptr_t> pcVtbl{ RE::VTABLE_PlayerCharacter[0] };
        PlayerUpdateHook::func = pcVtbl.write_vfunc(PlayerUpdateHook::idx, PlayerUpdateHook::thunk);

        spdlog::info("[hook] installed: Character Update(0x{:X}) + PlayerCharacter Update(0x{:X}). "
                     "Central 0xAD seat live for every NPC (authority re-asserted here).",
                     CharacterUpdateHook::idx, PlayerUpdateHook::idx);
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            InstallHooks();
            if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
                idm->AddEventSink(InputSink::GetSingleton());
                spdlog::info("[input] hotkeys armed (aim crosshair at a follower/NPC first):");
                spdlog::info("[input]   Numpad1 (0x{:X}) = MOVE engage (walk to player)   Numpad3 (0x{:X}) = MOVE disengage",
                             kKeyMoveEngage, kKeyMoveDisengage);
                spdlog::info("[input]   Numpad4 (0x{:X}) = LOOK-UP toggle                  Numpad7 (0x{:X}) = CROUCH toggle",
                             kKeyLookUpToggle, kKeyCrouchToggle);
            }
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            ReleaseAllFacets("kPreLoadGame");
            break;
        default: break;
        }
    }

}   // namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    SetupLog();

    const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    const auto  ver    = plugin->GetVersion();
    spdlog::info("=== APMF {}.{}.{} loading -- game {} ===",
                 ver.major(), ver.minor(), ver.patch(),
                 REL::Module::get().version().string());

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);

    spdlog::info("=== APMF loaded (prototype 1: animated move + look-up + crouch, held under authority) ===");
    return true;
}
