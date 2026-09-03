#include "PCH.h"
#include "core/Log.h"
#include "core/Input.h"
#include "core/Arbiter.h"
#include "core/Registry.h"
#include "core/AliasPkgProbe.h"
#include "core/T1Probe.h"
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
                    apmf::probe::OnHotkey(btn->GetIDCode());            // 0x49 probe: NumpadEnter/Numpad3 (shared claim set)
                    apmf::t1probe::OnHotkey(btn->GetIDCode());          // T1 probe: NumpadEnter/Numpad3/Numpad6 (shared claim set) / NumpadSlash (deny)
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
        spdlog::info("[input]   scancode 0x52 -> RELEASE ALL controlled NPCs (Numpad0) -- ALSO clears the probe claim set");
        spdlog::info("[input]   PROBES (throwaway, numpad hotkeys -- F-keys are occupied by the game/modlist, "
                     "see Docs/PROBE-ALLOWANCE.md): 0x9C NumpadEnter / 0x51 Numpad3 (nearest in-combat, no aim) = "
                     "toggle one actor in/out of the SHARED claim set (T1 observe + 0x49 package-offer, both read "
                     "it); 0x4D Numpad6 = claim ALL in-combat NPCs in range at once (or clear the set if non-empty) "
                     "-- the whole-battle visual test; 0xB5 NumpadSlash = T1 Phase-1 Attack-leaf deny toggle for "
                     "the WHOLE claim set (needs at least one claim first); 0x4F Numpad1 = toggle "
                     "kAttackingDisabled (native-bit); 0x50 Numpad2 = toggle kCastingDisabled (native-bit). "
                     "T4 (TESActionData::Process) is REMOVED -- collided with SCAR.dll, see "
                     "Docs/PROBE-ALLOWANCE.md.");
    }

}
