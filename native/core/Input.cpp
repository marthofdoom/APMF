#include "PCH.h"
#include "core/Log.h"
#include "core/Input.h"
#include "core/Arbiter.h"
#include "core/Registry.h"
#include "core/NativeBitProbe.h"
#include "core/NonAliasProbe.h"

// Win32 INI read for [Input] -- same hand-declared extern every other INI-gated
// file in this project uses (PCH does not pull in <Windows.h>).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

namespace apmf::input {

    namespace {

        // The whole keyboard surface is OPT-IN (see Input.h). Missing file/
        // section/key -> GetPrivateProfileIntA returns the default (0/OFF)
        // without erroring, so this is safe with no ini present at all.
        bool g_armed = false;

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
                    apmf::Arbiter::Get().DispatchHotkey(btn->GetIDCode());
                    apmf::nativebitprobe::OnHotkey(btn->GetIDCode());   // native-bit probe: Numpad1 / Numpad2
                    apmf::nonaliasprobe::OnHotkey(btn->GetIDCode());    // 0x49/0xDF observe toggle + RTTI dump
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

    }

    void Register() {
        if (GetPrivateProfileIntA("Input", "EnableTestSurface", 0, "Data/SKSE/Plugins/APMF.ini") == 0) {
            spdlog::info("[input] test surface NOT armed -- no keyboard sink registered. This is the "
                         "shipped default: no scancode can claim a channel, flip a native bit or toggle "
                         "the non-alias observe switch. Set [Input] EnableTestSurface=1 in "
                         "Data/SKSE/Plugins/APMF.ini to arm it for a test session.");
            return;
        }
        if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
            idm->AddEventSink(InputSink::GetSingleton());
            g_armed = true;
        } else {
            spdlog::warn("[input] BSInputDeviceManager unavailable -- test surface requested by "
                         "[Input] EnableTestSurface=1 but NOT armed.");
        }
    }

    void LogHelp() {
        if (!g_armed) return;
        spdlog::info("[input] MULTI-NPC test surface armed. Aim at an NPC + a key ADDS it to the controlled "
                     "set; aim another + a key adds it too; press a key again on an aimed NPC to remove it.");
        for (auto* ch : apmf::Registry::Get().All()) {
            for (const auto& hk : ch->Hotkeys()) {
                spdlog::info("[input]   scancode 0x{} -> ch.{} {} : {}",
                             apmf::log::Hex(hk.code, 2), ch->ChannelNo(), ch->Name(), hk.label);
            }
        }
        spdlog::info("[input]   scancode 0x52 -> RELEASE ALL controlled NPCs (Numpad0)");
        spdlog::info("[input]   ch.7/ch.9 test claims above carry NO param (category/package) -- a real client "
                     "drives them via APMF_RequestEx (APMF_API.h); the hotkeys only prove claim lifecycle.");
        spdlog::info("[input]   native-bit probe (throwaway, Docs/PROBE-ALLOWANCE.md): 0x4F Numpad1 = toggle "
                     "kAttackingDisabled on the aimed NPC; 0x50 Numpad2 = toggle kCastingDisabled. "
                     "T4 (TESActionData::Process) is REMOVED -- collided with SCAR.dll, see "
                     "Docs/PROBE-ALLOWANCE.md.");
        spdlog::info("[input]   non-alias package OBSERVE probe (throwaway, Docs/PROBE-NONALIAS-PACKAGE.md): "
                     "0x45 NumLock = toggle 0x49 (CheckForCurrentAliasPackage) + 0xDF (PutCreatedPackage) "
                     "observe logging, OFF by default; 0x46 ScrollLock = one-shot vtable/RTTI dump of the "
                     "crosshair-aimed actor. Every Numpad0-9/./+/-/*//Enter scancode is already claimed "
                     "above, so this uses the two adjacent lock keys instead.");
    }

}
