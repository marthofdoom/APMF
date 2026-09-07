#include "PCH.h"
#include "core/Log.h"
#include "core/Clock.h"
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
//
// PER-HAND (2026-09-0x, INVARIANTS #18): `a_this` is already a per-hand object
// (RE::Actor stores one MagicCaster per RE::MagicSystem::CastingSource), so this
// is the ONE T2 seat that can resolve a real hand for free via
// MagicCaster::GetCastingSource() (vtable slot 0x15, publicly declared on
// MagicCaster -- RE/M/MagicCaster.h). See Allowance::AllowedCastForHand.
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

        // ---- F6 (2026-09-06): MAKE THE CAST DENY OBSERVABLE -------------------
        // This gate had NO per-decision log at all -- only the install line -- so
        // the completeness of the cast deny could not be observed from APMF.log.
        // The 2026-09-06 deny audit had to record its own P1 row as "DENIED (code)
        // / UNOBSERVED (log)": correct by reading, never once seen executing. That
        // is precisely the trap CLAUDE.md principle 5 exists for (five seats were
        // built on a caster the engine never runs), so the deny now says so.
        //
        // Throttled, not per-frame: the SAME idiom core/CastClassify.cpp uses --
        // one leaf mutex, a (actor << 32 | subject) key, and the 1500 ms cadence
        // every other seat in this codebase logs at. Keying on the pair means a
        // burst that denies several DIFFERENT spells for one actor reports each of
        // them once, while the same spell re-deliberated inside the window is
        // suppressed. Combat-thread traffic; the lock is never held across a call
        // into the engine.
        constexpr std::uint64_t kLogThrottleMs = 1500;

        std::mutex                                       g_rlMx;
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastDenyLogMs;

        bool DenyLogDue(RE::FormID a_actor, RE::FormID a_subject) {
            const auto now = apmf::clock::MonotonicMs();
            const auto key = (static_cast<std::uint64_t>(a_actor) << 32) | static_cast<std::uint64_t>(a_subject);
            std::scoped_lock lk(g_rlMx);
            auto& last = g_lastDenyLogMs[key];
            if (now - last < kLogThrottleMs) return false;
            last = now;
            return true;
        }

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

            // Per-hand deny (INVARIANTS #18): `a_this` IS the hand-specific caster --
            // RE::Actor keeps a SEPARATE MagicCaster per casting source (left/right/
            // other/instant), and MagicCaster::GetCastingSource() (vtable slot 0x15,
            // declared on MagicCaster itself, an ordinary unhooked virtual call, same
            // safety as GetCasterAsActor above) reports exactly which one THIS
            // deliberation is for. kOther/kInstant (staves, non-hand casters) resolve
            // to kUnknown -- Allowance degrades those to the actor-wide floor.
            const auto src = a_this->GetCastingSource();
            const auto callerHand =
                (src == RE::MagicSystem::CastingSource::kLeftHand)  ? allowance::Hand::kLeft  :
                (src == RE::MagicSystem::CastingSource::kRightHand) ? allowance::Hand::kRight :
                                                                       allowance::Hand::kUnknown;

            // ---- H1 (2026-09-05 drive-chain review): APMF's OWN driven form
            // OVERRIDES the ch.8 narrow, it is not co-required with it. ----
            //
            // The blocker this fixes: a ch.8 kIntent_SelectSpell claim carries
            // `claim.form == the ORIGINAL spell`. As soon as APMF's own executor
            // mints a delivery-flip PROXY (a heal-an-ally cast, where the source
            // spell's delivery is kSelf), the caster deliberates on the PROXY
            // FormID -- a form the ch.8 claim never names -- so the ch.8 test below
            // returned FALSE and, because the two were AND-ed, vetoed the ch.8b
            // allowance that was correctly permitting spell||proxy all along.
            // (Historically that denied APMF's OWN driven cast with kMultipleCast, so
            // every PROXIED cast could never charge while un-proxied ones animated
            // fine -- the exact asymmetry the field saw. The drive is retired, but the
            // rule outlives it: with the engine seats, the AI ITSELF charges the proxy
            // through this same hook, so admitting the claim's proxy here is now what
            // lets the NPC's own cast get off the ground at all.)
            //
            // The override is deliberately NARROW: `CastClaimNamesForHand` is the
            // strict POSITIVE form (a kIntent_Cast claim must actually STAND, on
            // THIS hand, naming THIS exact spell-or-proxy). It never fires when
            // there is no cast claim, when the claim is degenerate, when it belongs
            // to the other hand, or for any other form -- so ch.8 keeps denying
            // everything it denied before. And this still only ever lets the
            // ENGINE's own answer stand (`engineSays`); APMF never invents a YES.
            //
            // Read the ch.8b answer ONCE and reuse it below (it was previously
            // evaluated twice, once in each branch). Same predicate, same inputs;
            // one read also means both branches necessarily agree with each other
            // and with the deny line's own report of why.
            const bool castAllows = allowance::AllowedCastForHand(fid, subjectForm, callerHand);
            if (castAllows && allowance::CastClaimNamesForHand(fid, subjectForm, callerHand))
                return engineSays;

            // ch.8 (cast-select exclusivity) AND ch.8b (cast-execution exclusivity)
            // must BOTH pass (design.md §3.5). ch.8 narrows the AI to its selected
            // spell; ch.8b narrows it to the client's executed cast spell/proxy while
            // a kIntent_Cast claim stands -- scoped to the claim's own hand (above),
            // so a single-hand cast claim leaves the OTHER hand's charge decision
            // untouched. Either narrowing to NO denies the charge.
            const bool selectAllows = allowance::Allowed(fid, APMF_API::kIntent_SelectSpell, subjectForm);
            if (selectAllows && castAllows)
                return engineSays;

            if (a_reason) *a_reason = RE::MagicSystem::CannotCastReason::kMultipleCast;

            // F6: the deny, observed. Names the actor, the spell it refused, the
            // hand this caster instance is for, and WHICH narrowing said no -- so a
            // log can distinguish "ch.8 select claim" from "ch.8b cast claim (this
            // hand)" without re-deriving it from the claim traffic. Throttled per
            // (actor, spell); nothing else about the decision changes.
            if (DenyLogDue(fid, subjectForm)) {
                spdlog::info("[t2c] 0x{} '{}' CheckCast DENIED spell=0x{} '{}' hand={} (ch.8 select {}, "
                             "ch.8b cast {}) -- engine had said YES; returned kMultipleCast so the AI "
                             "re-deliberates instead of charging this spell.",
                             apmf::log::Hex(fid), actor->GetName() ? actor->GetName() : "?",
                             apmf::log::Hex(subjectForm),
                             a_spell && a_spell->GetName() ? a_spell->GetName() : "?",
                             callerHand == allowance::Hand::kLeft  ? "L" :
                             callerHand == allowance::Hand::kRight ? "R" : "?",
                             selectAllows ? "ALLOW" : "DENY", castAllows ? "ALLOW" : "DENY");
            }
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
