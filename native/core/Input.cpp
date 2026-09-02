#include "PCH.h"
#include "core/Input.h"
#include "core/Arbiter.h"
#include "core/Registry.h"

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
        spdlog::info("[input] test-surface hotkeys armed (aim the crosshair at a follower/NPC first):");
        for (auto* ch : apmf::Registry::Get().All()) {
            for (const auto& hk : ch->Hotkeys()) {
                spdlog::info("[input]   scancode 0x{:02X} -> ch.{} {} : {}",
                             hk.code, ch->ChannelNo(), ch->Name(), hk.label);
            }
        }
    }

}
