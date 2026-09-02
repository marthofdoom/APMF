#include "PCH.h"

// ============================================================================
// APMF — AI Package Management Framework. PROTOTYPE 0.
//
// Purpose (design.md §9 #1/#2): prove the movement-layer hijack. Install the
// central Actor::Update(0xAD) vtable hook so every NPC routes through APMF, GATE
// to ONE tester-chosen target, and drive that body toward a marker through the
// MOVEMENT LAYER (Actor::Move 0xC8) WITHOUT ever touching the target's current
// package. Then observe, every tick, whether the package stays coherent while
// the body is commandeered, and whether it resumes cleanly on yield.
//
// This is an OBSERVATION prototype: heavy but rate-limited logging, one target,
// no package substitution. Version-robust: vtable-index hooks only, no
// hardcoded call-site offsets (the exact reason 0xAD/0xC8 are safe across
// runtimes — same thesis MFO's UpdateCombat hook and TDM's 0xAD hook rely on).
// ============================================================================

namespace {

    // ---- Log ---------------------------------------------------------------
    // Game-root-relative path so MO2/USVFS redirects it into the profile's
    // Overwrite folder, beside every other SKSE log (same trick as MFO). Falls
    // back to SKSE's log dir if that write is refused (running outside MO2).
    void SetupLog() {
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;
        try {
            sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("Data/SKSE/Plugins/APMF.log", true);
        } catch (const spdlog::spdlog_ex&) {
            if (auto dir = SKSE::log::log_directory()) {
                try {
                    sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "APMF.log").string(), true);
                } catch (const spdlog::spdlog_ex&) {
                    return;
                }
            } else {
                return;
            }
        }
        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);   // prototype: flush every line so a CTD keeps the trail
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    }

    // ---- Controls (DirectInput scancodes) ----------------------------------
    // Numpad keys, chosen to avoid common binds. To rebind, edit these and let
    // CI rebuild. ENGAGE captures the crosshair actor + the player's current
    // position as the marker; DISENGAGE yields the body back to its package.
    constexpr std::uint32_t kKeyEngage    = 0x4F;   // DIK_NUMPAD1
    constexpr std::uint32_t kKeyDisengage = 0x51;   // DIK_NUMPAD3

    // Stop redirecting once within this many world units of the marker.
    constexpr float kArriveRadius = 96.0f;

    // ---- Gated hijack state (all touched on the main/sim thread only) -------
    std::atomic<bool>        g_engaged{ false };
    std::atomic<RE::Actor*>  g_targetPtr{ nullptr };   // fast identity compare in the hot thunks
    RE::ActorHandle          g_targetHandle{};         // source-of-truth liveness (main thread)
    RE::NiPoint3             g_marker{};                // captured at engage
    RE::FormID               g_pkgAtEngage{ 0 };        // the package the target held when we engaged
    std::atomic<std::uint64_t> g_updateTicks{ 0 };      // rate-limit the per-frame observability log
    std::atomic<std::uint64_t> g_moveTicks{ 0 };

    const char* PkgTypeName(RE::TESPackage* a_pkg) {
        if (!a_pkg) return "<none>";
        const char* n = a_pkg->GetObjectTypeName();
        return n ? n : "<unnamed>";
    }

    void Disengage(const char* a_why) {
        if (!g_engaged.exchange(false)) return;
        auto* t = g_targetPtr.exchange(nullptr);
        RE::FormID id = t ? t->GetFormID() : 0;
        RE::TESPackage* pkg = t ? t->GetCurrentPackage() : nullptr;
        spdlog::info("[yield] DISENGAGE ({}) target=0x{:08X} pkg-now=0x{:08X}({}) pkg-at-engage=0x{:08X} :: "
                     "body released to its package -- watch it resume its ORIGINAL behaviour",
                     a_why, id, pkg ? pkg->GetFormID() : 0, PkgTypeName(pkg), g_pkgAtEngage);
        g_targetHandle = RE::ActorHandle{};
        g_pkgAtEngage = 0;
    }

    void Engage() {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) return;

        RE::Actor* target = nullptr;
        if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
            if (auto ref = pick->targetActor.get()) {
                target = ref->As<RE::Actor>();
            }
        }
        if (!target) {
            spdlog::warn("[engage] REFUSED -- no actor under the crosshair. Aim directly at a "
                         "follower/NPC, then press ENGAGE (Numpad1).");
            return;
        }
        if (target->IsPlayerRef()) {
            spdlog::warn("[engage] REFUSED -- crosshair is on the player.");
            return;
        }

        auto* pkg = target->GetCurrentPackage();
        g_marker        = pc->GetPosition();        // drive the body to where the player stands now
        g_targetHandle  = target->GetHandle();
        g_pkgAtEngage   = pkg ? pkg->GetFormID() : 0;
        g_targetPtr.store(target);
        g_engaged.store(true);
        g_updateTicks.store(0);
        g_moveTicks.store(0);

        spdlog::info("[engage] ENGAGE target=0x{:08X} '{}' pkg=0x{:08X}({}) marker=({:.1f},{:.1f},{:.1f}) "
                     "dist={:.1f} :: hijacking LOCOMOTION only -- package left untouched",
                     target->GetFormID(),
                     target->GetName() ? target->GetName() : "?",
                     g_pkgAtEngage, PkgTypeName(pkg),
                     g_marker.x, g_marker.y, g_marker.z,
                     target->GetPosition().GetDistance(g_marker));
    }

    // ---- Input sink: engage/disengage hotkeys ------------------------------
    class InputSink : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static InputSink* GetSingleton() {
            static InputSink s;
            return &s;
        }
        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
                                              RE::BSTEventSource<RE::InputEvent*>*) override {
            if (!a_event) return RE::BSEventNotifyControl::kContinue;
            for (auto* e = *a_event; e; e = e->next) {
                if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
                auto* btn = e->AsButtonEvent();
                if (!btn || !btn->IsDown()) continue;            // key-down edge only
                if (btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;
                switch (btn->GetIDCode()) {
                case kKeyEngage:    Engage();               break;
                case kKeyDisengage: Disengage("hotkey");    break;
                default: break;
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ---- The central hook: Actor::Update @ 0xAD ----------------------------
    // Patched ONCE on the Character vtable -> EVERY NPC routes through here (the
    // "one central point, track all NPCs" seat). We do observability for the
    // single gated target only; every other actor just calls the original.
    struct CharacterUpdateHook {
        static void thunk(RE::Actor* a_this, float a_delta) {
            func(a_this, a_delta);   // original FIRST, always -- we observe on top of the real tick
            if (!g_engaged.load(std::memory_order_relaxed)) return;
            if (a_this != g_targetPtr.load(std::memory_order_relaxed)) return;

            // Liveness: if the target unloaded, yield.
            if (!g_targetHandle || !g_targetHandle.get()) {
                Disengage("target-unloaded");
                return;
            }

            // ~ once/second at 60fps.
            if ((g_updateTicks.fetch_add(1, std::memory_order_relaxed) % 60) != 0) return;

            auto*     pkg  = a_this->GetCurrentPackage();
            RE::FormID id  = pkg ? pkg->GetFormID() : 0;
            const bool same = (id == g_pkgAtEngage);
            const float dist = a_this->GetPosition().GetDistance(g_marker);
            spdlog::info("[obs] tgt=0x{:08X} pkg=0x{:08X}({}) [{}] dist-to-marker={:.1f} hijack={}",
                         a_this->GetFormID(), id, PkgTypeName(pkg),
                         same ? "PACKAGE STABLE" : "PACKAGE CHANGED!!",
                         dist, dist > kArriveRadius ? "DRIVING" : "arrived");
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr std::size_t idx = 0x0AD;   // Actor::Update(float) — SE/AE, SKYRIM_REL_VR_VIRTUAL
    };

    // Player gets the same slot hooked purely as a heartbeat / proof the seat is
    // live; it never drives anything.
    struct PlayerUpdateHook {
        static void thunk(RE::PlayerCharacter* a_this, float a_delta) {
            func(a_this, a_delta);
            static bool s_first = true;
            if (s_first) { s_first = false; spdlog::info("[hook] player Update seat live (0xAD firing)"); }
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr std::size_t idx = 0x0AD;
    };

    // ---- The executor: Actor::Move @ 0xC8 ----------------------------------
    // Last-mile locomotion. Patched once on the Character vtable; for the gated
    // target while engaged we REDIRECT the engine's own per-frame movement delta
    // toward the marker (keeping the engine's chosen speed/magnitude, so motion
    // stays engine-natural), then hand it to the original. We NEVER read or
    // write the package here -- pure movement-layer override. Because we drive
    // the body AWAY from the package's own goal, a follow/travel package keeps
    // computing a path it never completes -> the truthful non-arrival of §5.
    struct MoveHook {
        static RE::bhkCharacterController* thunk(RE::Actor* a_this, float a_arg2, const RE::NiPoint3& a_pos) {
            if (!g_engaged.load(std::memory_order_relaxed) ||
                a_this != g_targetPtr.load(std::memory_order_relaxed)) {
                return func(a_this, a_arg2, a_pos);
            }

            RE::NiPoint3 toMarker = g_marker - a_this->GetPosition();
            toMarker.z = 0.0f;                         // steer on the horizontal plane
            const float dist = toMarker.Length();

            if (dist <= kArriveRadius) {
                // Arrived: stop steering, let the package's own delta through
                // (it will try to pull the body back toward its goal -- which is
                // exactly the yield we want to observe when close).
                return func(a_this, a_arg2, a_pos);
            }

            const float mag = a_pos.Length();          // the engine's chosen step for THIS frame
            RE::NiPoint3 dir = toMarker;
            dir.Unitize();
            RE::NiPoint3 redirected = dir * mag;
            redirected.z = a_pos.z;                     // preserve gravity/step component

            if ((g_moveTicks.fetch_add(1, std::memory_order_relaxed) % 120) == 0) {
                spdlog::info("[move] REDIRECT tgt=0x{:08X} dist={:.1f} step-mag={:.3f} "
                             "engine-delta=({:.3f},{:.3f},{:.3f}) -> apmf-delta=({:.3f},{:.3f},{:.3f})",
                             a_this->GetFormID(), dist, mag,
                             a_pos.x, a_pos.y, a_pos.z,
                             redirected.x, redirected.y, redirected.z);
            }
            return func(a_this, a_arg2, redirected);
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr std::size_t idx = 0x0C8;   // Actor::Move(float, const NiPoint3&)
    };

    std::atomic<bool> g_hooksInstalled{ false };

    void InstallHooks() {
        // VR relocates these vtable slots; writing 0xAD/0xC8 there would vector
        // every frame into an arbitrary virtual (instant CTD). Refuse, exactly
        // as MFO's Update/UpdateCombat hooks do.
        if (REL::Module::IsVR()) {
            spdlog::warn("[hook] VR runtime -- 0xAD/0xC8 indices unverified for VR; hooks NOT installed.");
            return;
        }
        if (g_hooksInstalled.exchange(true)) return;

        REL::Relocation<std::uintptr_t> charVtbl{ RE::VTABLE_Character[0] };
        CharacterUpdateHook::func = charVtbl.write_vfunc(CharacterUpdateHook::idx, CharacterUpdateHook::thunk);
        MoveHook::func            = charVtbl.write_vfunc(MoveHook::idx, MoveHook::thunk);

        REL::Relocation<std::uintptr_t> pcVtbl{ RE::VTABLE_PlayerCharacter[0] };
        PlayerUpdateHook::func = pcVtbl.write_vfunc(PlayerUpdateHook::idx, PlayerUpdateHook::thunk);

        spdlog::info("[hook] installed: Character Update(0x{:X}) + Move(0x{:X}), PlayerCharacter Update(0x{:X}). "
                     "Central 0xAD seat is live for every NPC.",
                     CharacterUpdateHook::idx, MoveHook::idx, PlayerUpdateHook::idx);
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            InstallHooks();
            if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
                idm->AddEventSink(InputSink::GetSingleton());
                spdlog::info("[input] hotkeys armed -- ENGAGE=Numpad1 (0x{:X}), DISENGAGE=Numpad3 (0x{:X}). "
                             "Aim at a follower, press ENGAGE, walk away, watch it get driven to your spot; "
                             "press DISENGAGE to yield.", kKeyEngage, kKeyDisengage);
            }
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            Disengage("kPreLoadGame");
            break;
        default:
            break;
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

    spdlog::info("=== APMF loaded (prototype 0: movement-hijack bench) ===");
    return true;
}
