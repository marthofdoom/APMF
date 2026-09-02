#include "PCH.h"
#include "core/Registry.h"
#include "core/Arbiter.h"

// ============================================================================
// Channel 8 -- CASTING SELECTION (not the trigger). Own selectedSpells[slot] so
// the AI's own caster casts the spell WE chose (design.md CHANNEL-MAP ch.8:
// "SELECT = TRUE gate"). A strict improvement over force-over-the-top: the AI
// still owns the trigger; we only set the input (which spell occupies the hand).
// The TRIGGER gate stays probe-gated (no documented suppressor) and is NOT built.
//
// Test facet: force Firebolt into the right hand. Release restores the prior
// selection. Package-independent.
// ============================================================================

namespace {

    constexpr RE::FormID kFirebolt = 0x0001C789;   // Skyrim.esm Firebolt (aimed, visible)

    class CastingSelectChannel final : public apmf::Channel {
    public:
        const char* Name() const override { return "casting-select"; }
        int         ChannelNo() const override { return 8; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4B, "Numpad4 : toggle own right-hand spell selection (Firebolt)" },
            };
            return keys;
        }

        void OnHotkey(std::uint32_t, RE::Actor* target) override {
            if (engaged.load()) { Release(target ? target : apmf::Arbiter::Get().CurrentTarget()); return; }
            if (!target) { spdlog::warn("[ch.8] REFUSED -- no gated target."); return; }
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(kFirebolt);
            if (!spell) { spdlog::warn("[ch.8] REFUSED -- Firebolt 0x{:08X} not found.", kFirebolt); return; }

            auto& rt      = target->GetActorRuntimeData();
            auto* existing = rt.selectedSpells[RE::Actor::SlotTypes::kRightHand];
            prevRight     = existing ? existing->As<RE::SpellItem>() : nullptr;   // exact-type-safe restore

            // OWN the right-hand selection. SetCurrentSpell is not bound at this
            // CommonLib rev, so write the slot + the caster's currentSpell member
            // directly (guarded) -- the AI then casts our spell itself.
            rt.selectedSpells[RE::Actor::SlotTypes::kRightHand] = spell;
            if (auto* caster = target->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand)) {
                caster->currentSpell = spell;
            }
            engaged.store(true);
            spdlog::info("[ch.8] engaged on 0x{:08X} -- right-hand selection := 0x{:08X} '{}' (prev 0x{:08X}). "
                         "Clean gate: AI casts our spell; trigger stays the AI's.",
                         target->GetFormID(), kFirebolt, spell->GetName() ? spell->GetName() : "?",
                         prevRight ? prevRight->GetFormID() : 0);
        }

        void Release(RE::Actor* actor) override {
            if (!engaged.exchange(false)) return;
            if (actor) {
                auto& rt = actor->GetActorRuntimeData();
                rt.selectedSpells[RE::Actor::SlotTypes::kRightHand] = prevRight;
                if (auto* caster = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand)) {
                    caster->currentSpell = prevRight;
                }
            }
            spdlog::info("[ch.8] released -- right-hand selection restored to 0x{:08X}.",
                         prevRight ? prevRight->GetFormID() : 0);
        }

    private:
        RE::SpellItem* prevRight = nullptr;
    };

}

APMF_REGISTER_CHANNEL(CastingSelectChannel);
