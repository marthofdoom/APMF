#include "PCH.h"
#include "core/Log.h"
#include "core/NativeBitProbe.h"

// ============================================================================
// See NativeBitProbe.h. Re-aims the crosshair on every press (a sticky claim
// would need co-save/kPreLoadGame handling for a flag that mutates a live
// Actor -- unnecessary for a throwaway toggle probe).
//
// DECK-PRESSABLE NUMPAD KEYS: F-keys aren't reachable on Steam Deck, and the
// two truly free numpad scancodes (NumpadEnter/NumpadSlash) are already
// spoken for by the shared T1/T4/0x49 claim key and T1's Phase-1 deny key
// (Docs/PROBE-ALLOWANCE.md). These two REPURPOSE existing channel-demo
// numpad keys for the duration of probing -- both keys still fire their
// normal channel action too (Input.cpp dispatches channel hotkeys and probe
// hotkeys unconditionally on every press), so avoid using NumpadStar/
// NumpadDot for ShoutPower/Equipment channel testing while these probes are
// armed:
//   - NumpadStar (0x37) also shadows ch. ShoutPower's "CLAIM the shout/power
//     facet" key.
//   - NumpadDot (0x53) also shadows ch. Equipment's "unequip right-hand
//     weapon" key.
// ============================================================================

namespace apmf::nativebitprobe {

    namespace {

        constexpr std::uint32_t kAttackKey = 0x37;   // NumpadStar (shadows ch. ShoutPower) -- toggle kAttackingDisabled
        constexpr std::uint32_t kCastKey   = 0x53;   // NumpadDot (shadows ch. Equipment) -- toggle kCastingDisabled

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
        if (g_installed.exchange(true)) return;
        spdlog::info("[nativebitprobe] ARMED (no hook, no VR gate -- a plain bit flip). NumpadStar (DIK 0x{}, also "
                     "shadows ch. ShoutPower) toggles kAttackingDisabled; NumpadDot (DIK 0x{}, also shadows "
                     "ch. Equipment) toggles kCastingDisabled on the crosshair-aimed NPC.",
                     apmf::log::Hex(kAttackKey, 2), apmf::log::Hex(kCastKey, 2));
    }

    void OnHotkey(std::uint32_t a_code) {
        if (!g_installed.load(std::memory_order_relaxed)) return;
        if (a_code == kAttackKey) ToggleBit(RE::Actor::BOOL_FLAGS::kAttackingDisabled, "kAttackingDisabled");
        else if (a_code == kCastKey) ToggleBit(RE::Actor::BOOL_FLAGS::kCastingDisabled, "kCastingDisabled");
    }

}
