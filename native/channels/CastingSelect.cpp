#include "PCH.h"
#include "core/Registry.h"

// ============================================================================
// Channel 8 -- CASTING SELECTION (not the trigger). Own selectedSpells[slot] so
// the AI's own caster casts the spell WE chose (CHANNEL-MAP ch.8: "SELECT = TRUE
// gate"). A strict improvement over force-over-the-top: the AI still owns the
// trigger; we only set the input (which spell occupies the hand). The TRIGGER gate
// stays a GAP (no documented suppressor) and is NOT built. Per-NPC prior selection
// captured for restore. Package-independent.
//
// Test facet: force Firebolt into the RIGHT hand. (Left-hand selection uses the
// identical path on SlotTypes::kLeftHand + the kLeftHand caster -- one API v2
// intent away; the code below is written to make that a one-line addition.)
// ============================================================================

namespace {

    constexpr RE::FormID kFirebolt = 0x0001C789;   // Skyrim.esm Firebolt (aimed, visible)

    class CastingSelectChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "casting-select"; }
        int              ChannelNo() const override { return 8; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_SelectSpell; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4B, "Numpad4 : toggle own right-hand spell selection (Firebolt)" },
            };
            return keys;
        }

        void Engage(RE::Actor* actor) override {
            if (!actor) return;
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(kFirebolt);
            if (!spell) { spdlog::warn("[ch.8] 0x{:08X} Firebolt not found.", actor->GetFormID()); return; }

            constexpr auto slot = RE::Actor::SlotTypes::kRightHand;
            auto& rt    = actor->GetActorRuntimeData();
            auto* exist = rt.selectedSpells[slot];
            m_prev[actor->GetFormID()] = exist ? exist->As<RE::SpellItem>() : nullptr;

            // SetCurrentSpell is unbound at this rev (INVARIANTS #8), so write the
            // slot + the caster's currentSpell member directly (guarded). The AI
            // then casts our spell itself.
            rt.selectedSpells[slot] = spell;
            if (auto* caster = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand))
                caster->currentSpell = spell;
            spdlog::info("[ch.8] 0x{:08X} right-hand selection := Firebolt (prev 0x{:08X}). Clean gate: AI "
                         "casts our spell; trigger stays the AI's.",
                         actor->GetFormID(), m_prev[actor->GetFormID()] ? m_prev[actor->GetFormID()]->GetFormID() : 0);
        }

        void Release(RE::Actor* actor) override {
            if (actor) {
                if (auto it = m_prev.find(actor->GetFormID()); it != m_prev.end()) {
                    constexpr auto slot = RE::Actor::SlotTypes::kRightHand;
                    actor->GetActorRuntimeData().selectedSpells[slot] = it->second;
                    if (auto* caster = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand))
                        caster->currentSpell = it->second;
                    spdlog::info("[ch.8] 0x{:08X} right-hand selection restored to 0x{:08X}.",
                                 actor->GetFormID(), it->second ? it->second->GetFormID() : 0);
                }
                m_prev.erase(actor->GetFormID());
            }
        }

    private:
        std::unordered_map<RE::FormID, RE::SpellItem*> m_prev;   // game-thread only
    };

}

APMF_REGISTER_CHANNEL(CastingSelectChannel);
