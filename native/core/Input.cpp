#include "PCH.h"
#include "core/Log.h"
#include "core/Input.h"
#include "core/Arbiter.h"
#include "core/Registry.h"
#include "core/NativeBitProbe.h"

namespace apmf::input {

    namespace {

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
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

    }

    void Register() {
        if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
            idm->AddEventSink(InputSink::GetSingleton());
        }
    }

    void LogHelp() {
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
    }

}
