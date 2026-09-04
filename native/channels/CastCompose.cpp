#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"
#include "channels/CastCompose.h"

// ============================================================================
// Channel 8b -- CAST EXECUTION (design.md §3, the keystone). A kIntent_Cast claim
// says: "for the next <= N ms, actor X's cast facet is mine -- spell S (and its
// runtime proxy P), at target T; keep the AI's own casting and re-arming out of
// the way; do not touch movement." APMF records the owner, DENIES the competitors
// at three gates it already owns, and auto-releases at the TTL. APMF FIRES NOTHING.
//
// THE BRIGHT LINE (design.md §1a / §3.7). APMF makes NO cast write for this facet:
// no CastSpellImmediate, no StartCharge/StartCast, no InterruptCast, no
// NotifyAnimationGraph("MRh_..."), no EquipSpell, no selectedSpells/desiredTarget
// write. The CLIENT executes its own animated cast through the hand ActorMagicCaster
// (MFO SPEC-FORCED-CAST.md). This channel is LOG-ONLY, exactly like CastingSelect.cpp
// (ch.8): the entire effect lives one layer down, in the SAME three gates ch.8
// already rides plus one ch.7 category bit, reading one more claim kind:
//   * core/CastGate.cpp   (T2c 0x0A CheckCast)        -- Allowance::AllowedCast
//   * core/EquipGate.cpp  (T2a 0x0F CheckShouldEquip)  -- Allowance::AllowedCast
//   * core/ActionGate.cpp (T1 cast leaves)             -- kCombatActionCat_Cast
// and the ControlMap Drain TTL pass (bounded auto-release, NOT a re-assert loop).
// No engine call is added anywhere; every touch is a deny/arbitrate the code already
// makes, now reading kIntent_Cast too. A cast is NEVER a package: PackageGate's 0x49
// thunk only reads kIntent_OfferPackage, so a kIntent_Cast claim is invisible to it.
// ============================================================================

namespace apmf::castcompose {

    bool ExtractFromPackage(RE::FormID pkgForm, RE::FormID& outSpell, RE::FormID& outTarget) {
        outSpell  = 0;
        outTarget = 0;
        if (pkgForm == 0) return false;

        // We NEVER run/offer/install/evaluate the package (design.md §3.7). This is a
        // pure READ of two fields. Confirm the form is really a TESPackage first.
        auto* pkg = RE::TESForm::LookupByID<RE::TESPackage>(pkgForm);
        if (!pkg) {
            spdlog::warn("[ch.8b] cast-from-package: 0x{} is not a loadable TESPackage.",
                         apmf::log::Hex(pkgForm));
            return false;
        }

        // FALLBACK (design.md §5.1/§6, INVARIANTS #7). The spell-typed package-data
        // input read that would recover S/T out of a UseMagic / MFO_CastPackage
        // package is NOT cleanly expressible in the pinned CommonLib: it requires the
        // hand-recovered BGSPackageDataPointerTemplate offsets MFO's Packages.cpp
        // carries (its `kInputSpell` name->uid->packageData walk + ReadTarget, note
        // MFO's own field bug: the authored input is "SPELL", MFO looked up "Spell").
        // Porting raw offsets APMF cannot static_assert against the pinned headers
        // would violate #7, so per §6 APMF ships the DIRECT-form path only and logs
        // the package form as unsupported -- NEVER letting the extraction convenience
        // block the facet. The client always has the spell in hand anyway: it should
        // call RequestCast with req.spell = the SpellItem directly (no FromPackage).
        // A future pass may drop a VERIFIED clean read here; until then this refuses,
        // and the claim is dropped -- never a package run.
        spdlog::warn("[ch.8b] cast-from-package: extraction UNSUPPORTED in this build for pkg 0x{} "
                     "-- pass the spell directly (kCastFlag_FromPackage refused; package untouched).",
                     apmf::log::Hex(pkgForm));
        return false;
    }

}

namespace {

    // Log-only ch.8b channel (design.md §3.6), exactly like CastingSelect.cpp.
    // Anonymous-namespace + file scope so APMF_REGISTER_CHANNEL's token-paste sees a
    // simple identifier (the macro cannot take a qualified name).
    class CastComposeChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "cast-execution"; }
        int              ChannelNo() const override { return 8; }   // ch.8b (sub-split of 8)
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_Cast; }

        void Engage(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.8b] 0x{} cast-execution CLAIMED (spell 0x{}). Bounded TTL, arbitration "
                         "+ deny only -- the CLIENT fires its own animated cast; APMF makes no cast write.",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.8b] 0x{} cast-execution claim RE-POINTED (spell 0x{}).",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            spdlog::info("[ch.8b] 0x{} cast-execution facet released.", apmf::log::Hex(id));
        }
        // No Tick: the deny lives entirely in the gate consults (INVARIANTS #1).
    };

}

APMF_REGISTER_CHANNEL(CastComposeChannel);
