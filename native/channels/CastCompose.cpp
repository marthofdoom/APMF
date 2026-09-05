#include "PCH.h"
#include "core/Log.h"
#include "core/Registry.h"
#include "core/CastProxy.h"
#include "core/MainThread.h"
#include "channels/CastCompose.h"

// ============================================================================
// Channel 8b -- CAST EXECUTION (design.md §3, the keystone). A kIntent_Cast claim
// says: "for the next <= N ms, actor X's cast facet is mine -- spell S (and its
// runtime proxy P), at target T." APMF records the owner, ANSWERS the engine's own
// cast-decision seats so the NPC's OWN AI performs that cast, DENIES the AI's
// competing choices at the gates it already owns, and auto-releases at the TTL.
//
// THE BRIGHT LINE HOLDS, AND IS NOW STRONGER (design.md §1a / §3.7). APMF makes NO
// cast write for this facet: no CastSpellImmediate, no StartCharge/StartCast, no
// InterruptCast, no NotifyAnimationGraph, no EquipSpell, no selectedSpells or
// desiredTarget write. Every one of those was in the RETIRED forced drive
// (core/CastExecutor.cpp, gone in feat/ai-cast-seats-impl) and none of them is made
// anywhere now. The cast is performed end to end by the engine's own behavior tree,
// on APMF's ANSWERS to five vfunc seats:
//   * core/EquipGate.cpp  (0x0F CheckShouldEquip)      -- WHICH item enters the hands
//   * core/CastSeats.cpp  (0x06 CheckStartCast)        -- WHETHER to start
//   * core/CastSeats.cpp  (0x0A GetMagicTarget)        -- WHERE it applies
//   * core/CastSeats.cpp  (0x07 CheckStopCast)         -- HOW LONG a channel runs
//   * core/CastSeats.cpp  (0x0D SetupAimController)    -- the projectile/facing aim
// plus the exclusivity denies the claim already rode:
//   * core/CastGate.cpp   (T2c 0x0A CheckCast)         -- Allowance::AllowedCastForHand
//   * core/EquipGate.cpp  (T2a 0x0F CheckShouldEquip)  -- Allowance::AllowedCastForHand
// and the ControlMap Drain TTL pass (bounded auto-release, NOT a re-assert loop).
// CastGate/EquipGate stay PER-HAND (feat/deny-perhand, INVARIANTS #18): each resolves
// its own hand from an engine-native signal (MagicCaster::GetCastingSource() /
// CombatInventoryItem::itemSlot.equipSlot) and narrows only the claim's own hand.
//
// NO LONGER PART OF THIS CLAIM: the ch.7 kCombatActionCat_Cast deny and the
// ContextMagic CreateContextNode deny. Both existed to keep the AI's magic branch
// silent while APMF drove the cast itself; the AI's magic branch IS the delivery
// mechanism now, so denying it would silence the claim's own cast. See
// core/ActionGate.cpp's RETIRED header block. A ch.7 kIntent_CombatAction claim can
// still deny casting outright -- that is a different, still-live intent.
//
// This channel itself stays LOG-ONLY plus ONE lifecycle duty: releasing the
// delivery-flip proxy (core/CastProxy.h) one main-thread hop AFTER the cleared claim
// publishes -- see Release() for why the ordering is load-bearing.
// A cast is NEVER a package: PackageGate's 0x49 thunk only reads
// kIntent_OfferPackage, so a kIntent_Cast claim is invisible to it.
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
            spdlog::info("[ch.8b] 0x{} cast-execution CLAIMED (spell 0x{}). Bounded TTL. The NPC's OWN AI "
                         "now selects/equips/charges/aims/fires this spell at the claimed target through the "
                         "engine seats -- APMF makes no equip, anim or cast write of any kind.",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* /*actor*/, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.8b] 0x{} cast-execution claim RE-POINTED (spell 0x{}).",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
        }

        void Release(RE::FormID id, RE::Actor* /*actor*/) override {
            spdlog::info("[ch.8b] 0x{} cast-execution facet released -- the seats stop answering the moment "
                         "the cleared claim publishes; the AI reverts to its own choice on its next "
                         "equipment rescore.", apmf::log::Hex(id));

            // RELEASE ORDERING (Docs/INVARIANTS.md #20). This runs INSIDE
            // ControlMap::Drain, on the writer thread, while the release is still only
            // in the writer's PRIVATE working copy -- the cleared claim has not been
            // Publish()ed yet, so a combat-thread seat can still see the old generation
            // for the rest of this frame. Un-teaching the delivery-flip proxy HERE would
            // therefore pull the form out from under a claim the seats still consider
            // live. So the teardown is deferred exactly one main-thread hop:
            // apmf::mainthread::Pump() runs in Arbiter::OncePerFrame IMMEDIATELY AFTER
            // Drain() returns, i.e. strictly after Publish(). By then every seat already
            // reads "no claim" and chains to the engine, and freeing the proxy can race
            // nothing. Same confirmed-main seat, so the AddSpell/RemoveSpell calls stay
            // legal (core/MainThread.h).
            apmf::mainthread::Post([id] { apmf::castproxy::Free(id); });
        }
        // No Tick: the claim's effect lives entirely in the seat/gate consults
        // (INVARIANTS #1) -- no re-assert, no per-frame write.
    };

}

APMF_REGISTER_CHANNEL(CastComposeChannel);
