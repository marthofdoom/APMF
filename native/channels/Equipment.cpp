#include "PCH.h"
#include "core/Registry.h"

// ============================================================================
// Channel 15 -- EQUIPMENT (CHANNEL-MAP ch.15: the melee-vs-ranged lever). Own the
// equipped set: removing an item DENIES the AI that option -- a clean INPUT-gate
// (ch.4/7/8 can only work with what is equipped). ActorEquipManager::UnequipObject
// / EquipObject are bound. Test facet: unequip her RIGHT-HAND weapon (deny it),
// re-equip it on release. Form-free -- we read what she has equipped, so nothing is
// hardcoded.
//
// These run on the GAME thread here (the ControlMap drains on the player 0xAD seat),
// so no MainThread::Post is needed (that scar -- MFO #62 -- is only for OFF-main
// equip calls). Per-NPC the removed weapon is captured for exact re-equip.
//
// SAVE-SAFETY (INVARIANTS #15): this channel mutates PERSISTED inventory (the
// unequip persists in the .ess) but the captured "re-equip on release" pointer is
// RAM-only -- it is NOT co-saved (unlike the AV ledger). So this channel is NOT
// save-safe: a client must NOT HOLD an equipment claim across a save. It self-heals
// in practice (the item stays in inventory and the AI re-equips its best weapon on
// entering combat / package eval), so a save-while-held leaves an item unequipped,
// not lost -- but do not rely on APMF to restore it. Only the AV channels
// (disposition/gait/detection) are co-saved and save-safe.
// ============================================================================

namespace {

    class EquipmentChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "equipment"; }
        int              ChannelNo() const override { return 15; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_Equipment; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x53, "NumpadDot : unequip right-hand weapon (deny it), restore on release" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* actor) override {
            if (!actor) return;
            auto* eqm = RE::ActorEquipManager::GetSingleton();
            if (!eqm) return;
            auto* eq   = actor->GetEquippedObject(false);            // right hand
            auto* weap = eq ? eq->As<RE::TESObjectWEAP>() : nullptr; // only weapons (a clean bound object)
            m_prev[id] = weap;
            if (weap) {
                eqm->UnequipObject(actor, weap);
                spdlog::info("[ch.15] 0x{:08X} unequipped right-hand weapon 0x{:08X} (denied). Restores on release.",
                             id, weap->GetFormID());
            } else {
                spdlog::info("[ch.15] 0x{:08X} no right-hand weapon to deny (no-op).", id);
            }
        }

        void Release(RE::FormID id, RE::Actor* actor) override {
            if (auto it = m_prev.find(id); it != m_prev.end()) {
                if (actor && it->second) {   // engine write only when the actor is live
                    if (auto* eqm = RE::ActorEquipManager::GetSingleton()) {
                        eqm->EquipObject(actor, it->second);
                        spdlog::info("[ch.15] 0x{:08X} re-equipped weapon 0x{:08X}.", id, it->second->GetFormID());
                    }
                }
                m_prev.erase(it);   // drop per-NPC state keyed by id, even if actor is null
            }
        }

    private:
        std::unordered_map<RE::FormID, RE::TESObjectWEAP*> m_prev;   // game-thread only
    };

}

APMF_REGISTER_CHANNEL(EquipmentChannel);
