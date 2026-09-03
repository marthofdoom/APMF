#include "PCH.h"
#include "core/Log.h"
#include "core/Input.h"
#include "core/Arbiter.h"
#include "core/Registry.h"
#include "core/AliasPkgProbe.h"
#include "core/T1Probe.h"
#include "core/T4Probe.h"
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
                    apmf::probe::OnHotkey(btn->GetIDCode());            // 0x49 probe: NumpadEnter (shared claim)
                    apmf::t1probe::OnHotkey(btn->GetIDCode());          // T1 probe: NumpadEnter (shared claim) / NumpadSlash (deny)
                    apmf::t4probe::OnHotkey(btn->GetIDCode());          // T4 probe: NumpadEnter (shared claim)
                    apmf::nativebitprobe::OnHotkey(btn->GetIDCode());   // native-bit probe: NumpadStar / NumpadDot
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
        spdlog::info("[input]   PROBES (throwaway, Deck-pressable numpad keys, Docs/PROBE-ALLOWANCE.md): "
                     "0x9C NumpadEnter = SHARED claim/release the aimed NPC (T1 observe + T4 observe + 0x49 "
                     "package-offer engage/release, all at once); 0xB5 NumpadSlash = T1 Phase-1 Attack-leaf "
                     "deny toggle (needs a claim first); 0x37 NumpadStar = toggle kAttackingDisabled "
                     "(native-bit, also shadows ch. ShoutPower's claim key while probing); 0x53 NumpadDot = "
                     "toggle kCastingDisabled (native-bit, also shadows ch. Equipment's unequip key while "
                     "probing).");
    }

}
