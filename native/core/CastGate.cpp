#include "PCH.h"
#include "core/Log.h"
#include "core/Allowance.h"
#include "core/CastGate.h"

// ============================================================================
// T2c -- CheckCast, the HARD pre-charge cast gate. Docs/ALLOWANCE-TEMPLATE.md
// §3/§7: the primary cast allowance, and the first template instance built --
// "solves the 3-cycle cast saga as the first template instance".
//
// Hooks MagicCaster::CheckCast (vtable slot 0x0A) on VTABLE_ActorMagicCaster[0]
// ONLY. ActorMagicCaster's other two vtable entries ([1] the
// SimpleAnimationGraphManagerHolder sub-object, [2] the
// BSTEventSink<BSAnimationGraphEvent> sub-object) are BASE-SUBOBJECT vtables of
// the SAME class at different sub-offsets, not separate casters -- patching them
// would clobber unrelated engine vtables. This is the identical symbol and the
// identical lesson MFO's own CasterConsent.cpp already documents (a caster
// deliberates through ALL its concrete CombatMagicCaster* categories, but
// CheckCast itself is declared once on MagicCaster and every caster resolves it
// through this one vtable slot 0).
//
// CheckCast fires BOTH in and out of combat (unlike the advisory
// CombatMagicCaster::CheckStartCast) and is, per field evidence (MFO
// CasterConsent.cpp / marth's deck), the gate that actually stops a spell from
// charging -- CheckStartCast alone leaks (a denied spell still fired). It is
// the ONE mechanism every ch.8 casting-select claim now rides: a follower with
// a live claim may only charge the claimed spell -- every other spell is
// denied at the gate, so the AI's own deliberation converges on the claim with
// no re-assert, no force, full animation. See channels/CastingSelect.cpp: ch.8
// itself still makes no engine write (still arbitration-only, the client
// selects+fires); THIS hook is what makes that claim a real allowance.
// ============================================================================

namespace apmf::castgate {

    namespace {

        using CheckCast_t = bool (*)(RE::MagicCaster*, RE::MagicItem*, bool, float*,
                                     RE::MagicSystem::CannotCastReason*, bool);

        // Originals keyed by vtable runtime address -- one entry today
        // (ActorMagicCaster[0]), but the map (not a single pointer) keeps this
        // symmetric with EquipGate.cpp / the template's general shape.
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_orig;
        std::atomic<bool> g_installed{ false };

        bool CheckCastThunk(RE::MagicCaster* a_this, RE::MagicItem* a_spell, bool a_dual,
                            float* a_cost, RE::MagicSystem::CannotCastReason* a_reason,
                            bool a_useBase) {
            // Recover the original for THIS vtable. A foreign object (this
            // thunk reached on a vtable we never installed on) gets the benign
            // default: allow, and touch nothing else on `a_this`.
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_orig.find(vt);
            if (oit == g_orig.end()) return true;
            const auto orig = reinterpret_cast<CheckCast_t>(oit->second);

            // Let the engine answer FIRST -- the template's core rule. If the
            // engine itself already says no, there is nothing to own.
            const bool engineSays = orig(a_this, a_spell, a_dual, a_cost, a_reason, a_useBase);
            if (!engineSays) return false;

            // Resolve the deliberating actor via the object's OWN (unhooked)
            // vtable slot 0x0C (MagicCaster::GetCasterAsActor) -- an ordinary
            // virtual call, safe because only slot 0x0A was patched.
            auto* actor = a_this->GetCasterAsActor();
            if (!actor) return engineSays;
            const auto fid         = actor->GetFormID();
            const auto subjectForm = a_spell ? a_spell->GetFormID() : 0;

            // ch.8 (cast-select exclusivity) AND ch.8b (cast-execution exclusivity)
            // must BOTH pass (design.md §3.5). ch.8 narrows the AI to its selected
            // spell; ch.8b narrows it to the client's executed cast spell/proxy while
            // a kIntent_Cast claim stands. Either narrowing to NO denies the charge.
            if (allowance::Allowed(fid, APMF_API::kIntent_SelectSpell, subjectForm) &&
                allowance::AllowedCast(fid, subjectForm))
                return engineSays;

            if (a_reason) *a_reason = RE::MagicSystem::CannotCastReason::kMultipleCast;
            return false;
        }

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[t2c] VR runtime -- the ActorMagicCaster vtable index is SE/AE-only "
                         "verified; CheckCast allowance NOT installed.");
            return;
        }
        if (g_installed.exchange(true)) return;

        // Expected RTTI base: MagicCaster (CheckCast is declared there; every
        // concrete caster, including ActorMagicCaster, derives it).
        REL::Relocation<void*> expectedTD{ RE::RTTI_MagicCaster };

        const REL::VariantID  kVtables[] = { RE::VTABLE_ActorMagicCaster[0] };
        constexpr std::size_t kCheckCast = 0x0A;

        const int n = allowance::InstallOnVtables(kVtables, kCheckCast, &CheckCastThunk,
                                                   expectedTD.get(), "t2c", g_orig);
        spdlog::info("[t2c] CheckCast allowance hooked on {} vtable(s) (ActorMagicCaster[0], "
                     "hard pre-charge gate) -- ch.8 casting-select claims now enforced here.", n);
    }

}
