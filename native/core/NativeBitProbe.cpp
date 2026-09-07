#include "PCH.h"
#include "core/Log.h"
#include "core/NativeBitProbe.h"

// Win32 INI read for [Probe.NativeBit] -- same hand-declared extern every other
// INI-gated file in this project uses (PCH does not pull in <Windows.h>).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// See NativeBitProbe.h. Re-aims the crosshair on every press (a sticky claim
// would need co-save/kPreLoadGame handling for a flag that mutates a live
// Actor -- unnecessary for a throwaway toggle probe).
//
// Standing convention: probe/test keys use the NUMPAD (see Docs/PROBE-
// ALLOWANCE.md) -- F-keys are occupied by the game/modlist, not usable here.
// Both opt-ins ([Probe.NativeBit] Enable and [Input] EnableTestSurface) default
// to OFF, so in a shipped game Numpad1/Numpad2 do nothing at all.
// ============================================================================

namespace apmf::nativebitprobe {

    namespace {

        constexpr std::uint32_t kAttackKey = 0x4F;   // Numpad1 -- toggle kAttackingDisabled on the aimed NPC
        constexpr std::uint32_t kCastKey   = 0x50;   // Numpad2 -- toggle kCastingDisabled on the aimed NPC

        std::atomic<bool> g_installed{ false };

        RE::Actor* CrosshairActor() {
            if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
                if (auto ref = pick->targetActor.get()) {
                    auto* a = ref->As<RE::Actor>();
                    if (a && !a->IsPlayerRef()) return a;
                }
            }
            return nullptr;
        }

        void ToggleBit(RE::Actor::BOOL_FLAGS a_flag, const char* a_label) {
            auto* actor = CrosshairActor();
            if (!actor) {
                spdlog::warn("[nativebitprobe] toggle REFUSED -- aim the crosshair at an NPC (not the player) first.");
                return;
            }
            auto& flags = actor->GetActorRuntimeData().boolFlags;
            const bool was = flags.any(a_flag);
            if (was) flags.reset(a_flag); else flags.set(a_flag);
            spdlog::info("[nativebitprobe] {} on 0x{} '{}': {} -> {}. Field-watch: does the actor cleanly stop/resume, "
                         "or wedge/stutter?", a_label, apmf::log::Hex(actor->GetFormID()),
                         actor->GetName() ? actor->GetName() : "?", was, !was);
        }

    }

    void Install() {
        // Config gate FIRST (see the .h): this probe writes to a live Actor, so it
        // stays completely inert unless a tester asks for it. Missing file/section/
        // key -> GetPrivateProfileIntA returns the default (0/OFF) without erroring.
        if (GetPrivateProfileIntA("Probe.NativeBit", "Enable", 0, "Data/SKSE/Plugins/APMF.ini") == 0) {
            spdlog::info("[nativebitprobe] NOT armed -- [Probe.NativeBit] Enable=0 in "
                         "Data/SKSE/Plugins/APMF.ini is the shipped default (this probe MUTATES a live "
                         "actor's bool flags, so it never arms itself).");
            return;
        }
        if (g_installed.exchange(true)) return;
        spdlog::info("[nativebitprobe] ARMED by [Probe.NativeBit] Enable=1 (no hook, no VR gate -- a plain "
                     "bit flip). Numpad1 (DIK 0x{}) toggles kAttackingDisabled; Numpad2 (DIK 0x{}) toggles "
                     "kCastingDisabled on the crosshair-aimed NPC. The keys only arrive if the keyboard "
                     "surface is ALSO armed ([Input] EnableTestSurface=1).",
                     apmf::log::Hex(kAttackKey, 2), apmf::log::Hex(kCastKey, 2));
    }

    void OnHotkey(std::uint32_t a_code) {
        if (!g_installed.load(std::memory_order_relaxed)) return;
        if (a_code == kAttackKey) ToggleBit(RE::Actor::BOOL_FLAGS::kAttackingDisabled, "kAttackingDisabled");
        else if (a_code == kCastKey) ToggleBit(RE::Actor::BOOL_FLAGS::kCastingDisabled, "kCastingDisabled");
    }

}
