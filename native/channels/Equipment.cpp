#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 15 -- EQUIPMENT (CHANNEL-MAP ch.15: the melee-vs-ranged lever). Own the
// equipped set: removing an item DENIES the AI that option -- a clean INPUT-gate
// (ch.4/7/8 can only work with what is equipped). ActorEquipManager::UnequipObject
// / EquipObject are bound. Probe facet (no param, the hotkey path): unequip her
// RIGHT-HAND weapon (deny it), re-equip it on release. Form-free -- we read what
// she has equipped, so nothing is hardcoded.
//
// GATE-ONLY facet (2026-09-03, param.form set -- the client-facing contract): a
// client that already holds/equips a weapon itself (e.g. MFO's own weapon-order)
// RequestEx's this intent with param.form = that weapon's FormID purely to
// ENGAGE the channel -- exactly ch.8's "arbitration + claim lifecycle only, real
// enforcement lives one layer down" shape (CastingSelect.cpp). The actual DENY
// enforcement for this facet lives in core/EquipGate.cpp's T2a CheckShouldEquip
// hook (it reads this SAME claim via Allowed(kIntent_Equipment, ..)); Engage/
// Release here make NO engine write for a param'd claim, so APMF never fights the
// client over which weapon is in hand -- it only narrows the AI's own equip
// choice while the claim stands, same non-generative discipline as every other
// T2-backed channel. The no-param probe path is UNCHANGED (still actively
// unequips/re-equips for hotkey testing).
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
// (disposition/gait/detection) are co-saved and save-safe. The GATE-ONLY facet
// above never mutates inventory at all, so this caveat does not apply to it.
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

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            // GATE-ONLY claim (param.form set): the client already holds/equips
            // the weapon itself; make NO engine write here. T2a's EquipGate.cpp
            // reads this claim directly for its deny decision -- see the header
            // comment above. Do not touch m_prev either, so Release() below is
            // correctly a no-op for this claim (nothing to restore).
            if (param.form != 0) {
                spdlog::info("[ch.15] 0x{} equipment facet CLAIMED gate-only (held item 0x{}). "
                             "Arbitration only -- T2a's CheckShouldEquip hook does the enforcing.",
                             apmf::log::Hex(id), apmf::log::Hex(param.form));
                return;
            }

            // Probe path (no param, the hotkey test facet): actively unequip the
            // right-hand weapon and restore it on release. Unchanged.
            if (!actor) return;
            auto* eqm = RE::ActorEquipManager::GetSingleton();
            if (!eqm) return;
            auto* eq   = actor->GetEquippedObject(false);            // right hand
            auto* weap = eq ? eq->As<RE::TESObjectWEAP>() : nullptr; // only weapons (a clean bound object)
            m_prev[id] = weap;
            if (weap) {
                eqm->UnequipObject(actor, weap);
                spdlog::info("[ch.15] 0x{} unequipped right-hand weapon 0x{} (denied). Restores on release.",
                             apmf::log::Hex(id), apmf::log::Hex(weap->GetFormID()));
            } else {
                spdlog::info("[ch.15] 0x{} no right-hand weapon to deny (no-op).", apmf::log::Hex(id));
            }
        }

        void Release(RE::FormID id, RE::Actor* actor) override {
            if (auto it = m_prev.find(id); it != m_prev.end()) {
                if (actor && it->second) {   // engine write only when the actor is live
                    if (auto* eqm = RE::ActorEquipManager::GetSingleton()) {
                        eqm->EquipObject(actor, it->second);
                        spdlog::info("[ch.15] 0x{} re-equipped weapon 0x{}.", apmf::log::Hex(id), apmf::log::Hex(it->second->GetFormID()));
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
