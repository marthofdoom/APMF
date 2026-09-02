#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"

// ============================================================================
// Channel 8 -- CASTING SELECTION (not the trigger). Own selectedSpells[slot] so
// the AI's own caster casts the spell WE chose (CHANNEL-MAP ch.8: "SELECT = TRUE
// gate"). A strict improvement over force-over-the-top: the AI still owns the
// trigger; we only set the input (which spell occupies the hand). The TRIGGER gate
// stays a GAP (no documented suppressor) and is NOT built. Per-NPC prior selection
// captured for restore. Package-independent.
//
// PARAM (API v2). The spell is the client's choice: RequestEx passes it as
// APMF_Param.form (a SpellItem FormID). This is the seam MFO drives -- a "cast X"
// gambit hands us X, and the follower's own AI casts X (fixing "gambit targets
// right but the AI casts its own spell"). With NO param (form == 0, e.g. the v1
// Request path or the Numpad4 test hotkey) we fall back to Firebolt so legacy
// behavior is preserved. Left-hand selection is the identical path on
// SlotTypes::kLeftHand + the kLeftHand caster -- one API intent away.
// ============================================================================

namespace {

    constexpr RE::FormID kFirebolt = 0x0001C789;   // Skyrim.esm Firebolt (aimed, visible) -- no-param default

    class CastingSelectChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "casting-select"; }
        int              ChannelNo() const override { return 8; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_SelectSpell; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0x4B, "Numpad4 : toggle own right-hand spell selection (no-param -> Firebolt)" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            if (!actor) return;
            auto* spell = ResolveSpell(id, param);
            if (!spell) return;   // ResolveSpell logged the reason

            constexpr auto slot = RE::Actor::SlotTypes::kRightHand;
            auto& rt    = actor->GetActorRuntimeData();
            auto* exist = rt.selectedSpells[slot];
            auto* prev  = exist ? exist->As<RE::SpellItem>() : nullptr;
            m_prev[id]  = prev;   // capture the pre-APMF selection ONCE (Engage only)

            SelectInto(actor, spell);
            spdlog::info("[ch.8] 0x{} right-hand selection := 0x{} (prev 0x{}). Clean gate: AI casts "
                         "our spell; trigger stays the AI's.",
                         apmf::log::Hex(id), apmf::log::Hex(spell->GetFormID()),
                         apmf::log::Hex(prev ? prev->GetFormID() : 0));
        }

        // A new higher-basis claim (or a new winner after the owner released) wants a
        // DIFFERENT spell. Switch the live selection WITHOUT touching m_prev -- the
        // original pre-APMF selection was captured at Engage and Release still restores
        // it. This makes the channel correct when two clients contend for the hand.
        void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            if (!actor) return;
            auto* spell = ResolveSpell(id, param);
            if (!spell) return;
            SelectInto(actor, spell);
            spdlog::info("[ch.8] 0x{} right-hand selection RE-POINTED to 0x{} (new owner; prior restore state kept).",
                         apmf::log::Hex(id), apmf::log::Hex(spell->GetFormID()));
        }

        void Release(RE::FormID id, RE::Actor* actor) override {
            if (auto it = m_prev.find(id); it != m_prev.end()) {
                if (actor) {   // engine write only when the actor is live
                    constexpr auto slot = RE::Actor::SlotTypes::kRightHand;
                    actor->GetActorRuntimeData().selectedSpells[slot] = it->second;
                    if (auto* caster = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand))
                        caster->currentSpell = it->second;
                    spdlog::info("[ch.8] 0x{} right-hand selection restored to 0x{}.",
                                 apmf::log::Hex(id), apmf::log::Hex(it->second ? it->second->GetFormID() : 0));
                }
                m_prev.erase(it);   // drop per-NPC state keyed by id, even if actor is null
            }
        }

    private:
        // The client's spell (param.form) if given, else the Firebolt fallback.
        RE::SpellItem* ResolveSpell(RE::FormID id, const APMF_API::APMF_Param& param) {
            const RE::FormID want = param.form ? param.form : kFirebolt;
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(want);
            if (!spell) spdlog::warn("[ch.8] 0x{} spell 0x{} not found (or not a SpellItem).",
                                     apmf::log::Hex(id), apmf::log::Hex(want));
            return spell;
        }

        // Set the right-hand slot + the caster's currentSpell directly. SetCurrentSpell
        // is unbound at this rev (INVARIANTS #8), so we write the slot the AI reads; the
        // AI then casts our spell itself.
        static void SelectInto(RE::Actor* actor, RE::SpellItem* spell) {
            constexpr auto slot = RE::Actor::SlotTypes::kRightHand;
            actor->GetActorRuntimeData().selectedSpells[slot] = spell;
            if (auto* caster = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand))
                caster->currentSpell = spell;
        }

        std::unordered_map<RE::FormID, RE::SpellItem*> m_prev;   // game-thread only
    };

}

APMF_REGISTER_CHANNEL(CastingSelectChannel);
