#include "PCH.h"
#include "core/Log.h"
#include "core/CastExecutor.h"
#include "core/ControlMap.h"
#include "core/MainThread.h"

#include <unordered_map>

// ============================================================================
// See CastExecutor.h for the contract + design. This TU is the drive: hand
// resolution, the delivery-flip proxy pool, the observed-sequence phase chain
// (PhaseSelect -> PhaseFire -> teardown), and the guaranteed-delivery fallback.
//
// REFERENCE (ported, not copied verbatim -- adapted to APMF's own architecture
// and GRADUATED from observe-only to actually owning delivery): MFO repo,
// branch `feat/forced-cast-trigger`, `native/ComposedCast.cpp`'s
// `DriveObservedCast`/`PhaseSelect`/`PhaseFire`/`DriveTeardown`/`HealProxy`. That
// implementation stayed OBSERVE-ONLY (always degraded to the caller's own
// kInstant apply) because it crashed when MFO's OWN force-equip raced the
// AI on the SAME hand -- exactly the UAF this repo's feat/deny-perhand pass
// root-caused and fixed. With MFO's force-equip REMOVED and APMF the SOLE
// equipper under its own per-hand deny (CastGate.cpp/EquipGate.cpp/
// ActionGate.cpp), that race is structurally gone, so this pass graduates the
// SAME sequence to actually own the cast (equip + drive + guaranteed fallback)
// rather than just observing it.
//
// THREADING (INVARIANTS #4/#12). Engage/OnOwnerChanged/Release run on the
// CONFIRMED-MAIN Drain seat (core/Hook.cpp's PlayerUpdateHook) -- direct engine
// mutation there is safe. The multi-frame phase continuations run via
// core/MainThread.h's Post/Pump, drained from that SAME seat
// (Arbiter::OncePerFrame, right after Drain()) -- so `g_drives` below is
// touched ONLY on that one confirmed thread and needs no lock, the same
// writer-thread-only pattern every other per-NPC state map in this codebase
// uses.
// ============================================================================

namespace apmf::castexec {

    namespace {

        using CS = RE::MagicSystem::CastingSource;

        // The numeric charge-state gate below assumes the engine's kNone==0
        // baseline (ComposedCast.cpp's own deck-observed thresholds: 0 = at rest,
        // >=3 = Charged/Casting -- the NAMED enumerators past kNone are unreliable
        // guesses in the pinned CommonLib and are deliberately NOT relied on here,
        // only the raw integer thresholds MFO's own [castobs] observer proved).
        static_assert(static_cast<std::uint32_t>(RE::MagicCaster::State::kNone) == 0,
                      "MagicCaster::State baseline moved -- re-check the raw-int "
                      "charge-state gates below (PhaseFire/PhaseSelect)");

        constexpr int   kSelectTries         = 6;     // frames to wait for the equip to select
        constexpr int   kChargePolls         = 180;   // frames (~3s) to wait for Charged before degrading
        constexpr int   kConcentrationHoldPolls = 180;   // frames (~3s) a concentration spell CHANNELS
                                                          // after firing, so it heals/applies over a real
                                                          // window instead of one instantaneous pulse
                                                          // (2026-09-05 field fix -- see PhaseHold).
        constexpr float kDriveBasis  = 1000.0f;   // internal ch.8b protection claim -- always wins

        // ---- Vanilla Left/Right Hand BGSEquipSlot, resolved once (lazy). Same
        // route EquipGate.cpp's per-hand deny uses (BGSDefaultObjectManager --
        // the SAME table the engine itself consults; never a hardcoded FormID). ----
        const RE::BGSEquipSlot* HandSlot(bool left) {
            static std::atomic<RE::BGSEquipSlot*> s_left{ nullptr };
            static std::atomic<RE::BGSEquipSlot*> s_right{ nullptr };
            auto& cache = left ? s_left : s_right;
            if (auto* cached = cache.load(std::memory_order_relaxed)) return cached;
            auto* dom = RE::BGSDefaultObjectManager::GetSingleton();
            if (!dom) return nullptr;
            auto* resolved = dom->GetObject<RE::BGSEquipSlot>(
                left ? RE::DEFAULT_OBJECT::kLeftHandEquip : RE::DEFAULT_OBJECT::kRightHandEquip);
            if (resolved) cache.store(resolved, std::memory_order_relaxed);
            return resolved;
        }

        // ---- Dedicated delivery-flip proxy pool (per-owner). TRANSIENT AddSpell
        // (2026-09-05 field fix): a runtime dynamic form, shared Effect* array
        // with the source. Main-thread-only (same seat as everything else here).
        // One slot per owner covers dual-hand too: both hands of a kHandDual
        // drive share the SAME spell, so one proxy form can be equipped in both
        // hands at once, exactly like a vanilla dual-cast.
        //
        // FIELD BUG (deck 2026-09-05): `ActorEquipManager::EquipSpell` can only
        // SELECT a spell the actor actually KNOWS (`Actor::HasSpell`) -- it does
        // NOT teach one. The proxy was never `AddSpell`'d, so the caster never
        // selected it (`currentSpell != castForm` forever) -> PhaseSelect always
        // exhausted its tries -> ALWAYS degraded to the fallback, every single
        // pulse (equip/interrupt/re-equip churn, no animation, ever). Fixed by
        // teaching the proxy to the actor on Acquire and un-teaching it on Free
        // -- TRANSIENT (never persisted as a real learned spell; added/removed
        // around the SAME window the form is equipped) so it never pollutes the
        // actor's real spell list. Guaranteed removal: `Free` is the ONE choke
        // point every teardown path in this file already calls unconditionally
        // (see TeardownHand) -- no path can equip the proxy without eventually
        // routing through here, so no path can leak it (the light-limit CTD
        // lesson: a leaked equipped/known transient spell must never survive a
        // save/teardown). ----
        namespace proxy {
            struct Slot { RE::SpellItem* form = nullptr; RE::FormID source = 0; RE::FormID owner = 0; };
            Slot g_slot[4];

            void Configure(RE::SpellItem* a_p, RE::SpellItem* a_src) {
                a_p->data          = a_src->data;                                // castingType/cost/etc.
                a_p->data.delivery = RE::MagicSystem::Delivery::kTargetActor;    // the ONLY change
                a_p->effects.clear();
                for (auto* e : a_src->effects) a_p->effects.push_back(e);        // shared source Effect*
            }
            RE::SpellItem* Acquire(RE::FormID a_owner, RE::SpellItem* a_src) {
                if (!a_src || !a_owner) return nullptr;
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_owner);
                if (!actor) return nullptr;   // no actor to teach the proxy to -- can't be made castable

                const auto sid = a_src->GetFormID();
                for (auto& s : g_slot) if (s.owner == a_owner && s.form) {
                    if (s.source != sid) Configure(s.form, a_src);   // re-target: re-share the new effects
                    s.source = sid;
                    if (!actor->HasSpell(s.form)) actor->AddSpell(s.form);   // TRANSIENT teach -- makes it selectable
                    return s.form;
                }
                for (auto& s : g_slot) if (s.owner == 0) {
                    if (!s.form) {
                        auto* f = RE::IFormFactory::GetConcreteFormFactoryByType<RE::SpellItem>();
                        s.form = f ? static_cast<RE::SpellItem*>(f->Create()) : nullptr;
                        if (!s.form) return nullptr;
                    }
                    Configure(s.form, a_src); s.source = sid; s.owner = a_owner;
                    actor->AddSpell(s.form);   // TRANSIENT teach -- makes it selectable (removed in Free)
                    return s.form;
                }
                spdlog::warn("[ch.8+act] proxy pool overflow (owner 0x{}) -- driving the "
                             "original form (self-pose, imperfect but never a crash).",
                             apmf::log::Hex(a_owner));
                return nullptr;
            }
            void Free(RE::FormID a_owner) {
                for (auto& s : g_slot) if (s.owner == a_owner) {
                    if (s.form) {
                        if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_owner))
                            actor->RemoveSpell(s.form);   // un-teach -- never left known/castable
                    }
                    s.owner = 0; s.source = 0;
                }
            }
            RE::FormID FormForOwner(RE::FormID a_owner) {
                for (auto& s : g_slot) if (s.owner == a_owner && s.form) return s.form->GetFormID();
                return 0;
            }
        }

        // ---- Per-actor, per-hand drive state. Writer-thread-only (see file
        // header) -- no lock. ----
        struct HandDrive {
            bool              active   = false;   // this hand is ours (in-flight OR parked after a fire)
            bool              inFlight = false;   // true from start until fired/failed (still mid-cast)
            RE::FormID        spell    = 0;   // the client's requested spell
            RE::FormID        castForm = 0;   // what's actually driven (proxy or spell)
            RE::FormID        target   = 0;   // 0 == self
            std::uint64_t     epoch    = 0;   // bumped on (re)start; cancels stale posts
            APMF_API::Handle  claim    = APMF_API::kInvalidHandle;   // internal ch.8b protection
        };
        struct ActorDrive { HandDrive hand[2]; };   // [0] = right, [1] = left
        std::unordered_map<RE::FormID, ActorDrive> g_drives;
        std::atomic<std::uint64_t> g_epoch{ 1 };

        // Per-drive context threaded (by value) through the main-thread phase chain.
        struct DriveCtx {
            RE::FormID    fid      = 0;
            RE::FormID    spell    = 0;
            RE::FormID    castForm = 0;
            RE::FormID    target   = 0;   // 0 == self
            bool          left     = false;
            std::uint64_t epoch    = 0;
        };

        // The live HandDrive for this context, or null if it was cancelled/
        // superseded (epoch mismatch, or the actor entry is gone entirely --
        // Release() erases it). Never persisted across a Post/frame boundary --
        // every phase function re-resolves this fresh at the top of each call.
        HandDrive* Live(const DriveCtx& c) {
            auto it = g_drives.find(c.fid);
            if (it == g_drives.end()) return nullptr;
            HandDrive& hd = c.left ? it->second.hand[1] : it->second.hand[0];
            if (!hd.active || hd.epoch != c.epoch) return nullptr;
            return &hd;
        }

        // Target resolution -- NEVER invents one (INVARIANTS #0/#17). See
        // CastExecutor.h's file header for the full rule. Priority (marth
        // 2026-09-05, the heal-the-player fix): the claim's OWN explicit
        // `param.target` first -- this is the ONLY correct source for
        // heal/buff-another, since the actor's combat TARGET is its FOE, not
        // whoever it should be healing (claiming CombatTarget=ally would
        // redirect the actor to FIGHT the ally). Only when the claim names no
        // target do we fall back to the actor's own winning kIntent_CombatTarget
        // claim (the offense case: cast at your foe), then self.
        RE::FormID ResolveTarget(RE::Actor* a_actor, RE::FormID a_explicitTarget) {
            if (a_explicitTarget != 0) return a_explicitTarget;   // the claim named WHO explicitly
            APMF_API::APMF_Param claim{};
            if (apmf::ControlMap::Get().TryGetOwningClaim(a_actor->GetFormID(),
                                                          APMF_API::kIntent_CombatTarget, claim) &&
                claim.form != 0)
                return claim.form;   // no explicit target -- fall back to the combat-target claim
            return a_actor->GetFormID();   // neither -- self (conservative; never a guess)
        }

        // Guaranteed-delivery fallback (marth's rule #5): a direct effect apply
        // via the kInstant caster, bypassing the animated drive entirely. Uses
        // whatever form was ALREADY resolved as castForm (the proxy if the
        // delivery was flipped, so a self-delivery heal still reaches the ally
        // even through this path).
        void FireFallback(const DriveCtx& c) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(c.fid);
            auto* form  = RE::TESForm::LookupByID<RE::SpellItem>(c.castForm);
            if (!actor || !form) return;
            auto* target = c.target ? RE::TESForm::LookupByID<RE::Actor>(c.target) : actor;
            if (!target) target = actor;
            if (auto* caster = actor->GetMagicCaster(CS::kInstant))
                caster->CastSpellImmediate(form, false, target, 1.0f, false, 0.0f, actor);
            spdlog::info("[ch.8+act] 0x{} fallback CastSpellImmediate(0x{}) -- guaranteed delivery.",
                         apmf::log::Hex(c.fid), apmf::log::Hex(c.castForm));
        }

        // Every phase exit that ends the drive on this hand funnels here: stop the
        // engine channel, play the observed teardown anims, deselect the driven
        // form, release the internal protection claim, and free the proxy slot
        // (which un-teaches it via `proxy::Free` -- see the proxy namespace's
        // header) -- UNLESS the sibling hand (a kHandDual drive shares ONE proxy
        // across both hands, since the proxy pool is keyed by owner, not hand) is
        // still actively using that same proxy form. Idempotent-safe on a null
        // actor.
        void TeardownHand(const DriveCtx& c) {
            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(c.fid)) {
                if (auto* hand = actor->GetMagicCaster(c.left ? CS::kLeftHand : CS::kRightHand))
                    hand->InterruptCast(false);   // stop the engine channel (no refund)
                actor->NotifyAnimationGraph("InterruptCast");
                actor->NotifyAnimationGraph("CastStop");
                if (auto* px = RE::TESForm::LookupByID<RE::SpellItem>(c.castForm))
                    actor->DeselectSpell(px);   // no lingering equipped spell (light-limit CTD lesson)
            }
            if (HandDrive* hd = Live(c)) {
                apmf::ControlMap::Get().EnqueueRelease(hd->claim);
                hd->claim  = APMF_API::kInvalidHandle;
                hd->active = false;
            }
            bool siblingStillUsesProxy = false;
            if (const RE::FormID proxyFid = proxy::FormForOwner(c.fid); proxyFid != 0) {
                if (auto it = g_drives.find(c.fid); it != g_drives.end()) {
                    const HandDrive& sibling = c.left ? it->second.hand[0] : it->second.hand[1];
                    siblingStillUsesProxy = sibling.active && sibling.castForm == proxyFid;
                }
            }
            if (!siblingStillUsesProxy) proxy::Free(c.fid);
        }

        // Forward decl -- PhaseSelect/PhaseFire/PhaseHold call each other across
        // the chain.
        void PhaseSelect(DriveCtx c, int triesLeft);

        // Releases the internal ch.8b protection claim and PARKS the hand --
        // `hd.active` stays true (this hand is still ours: Release() must still
        // deselect/restore it later) but there is nothing left to protect (the
        // fire/hold already happened), so the claim is no longer load-bearing. A
        // repeat Repoint re-drives from rest via StartHandDrive, which tears this
        // parked state down (deselect included) before re-equipping -- never a
        // second, competing claim while parked, never a lingering equipped spell
        // if Release() comes instead (INVARIANTS #18-style completeness for THIS
        // channel's own cleanup, marth's rule #6).
        void ParkHand(const DriveCtx& c) {
            if (HandDrive* hd = Live(c)) {
                apmf::ControlMap::Get().EnqueueRelease(hd->claim);
                hd->claim    = APMF_API::kInvalidHandle;
                hd->inFlight = false;   // parked -- a same-spell Repoint now fires again
            }
        }

        // Phase 3 (concentration only, 2026-09-05 field fix): after the initial
        // SpellFire, a concentration spell's magnitude is applied by the ENGINE's
        // OWN per-frame caster update for as long as it stays in the charged/
        // casting state (>=3) -- exactly like a player holding the cast button.
        // So we do NOT interrupt/re-fire here, just keep the channel (and the
        // internal ch.8b claim protecting the hand) alive by polling, up to a
        // bounded hold window (kConcentrationHoldPolls, ~3s) or until the caster
        // exits the state on its own -- whichever comes first -- then parks
        // (single instant-apply fallback semantics don't apply here: the spell
        // already fired at least once, so there is nothing to degrade to).
        void PhaseHold(DriveCtx c, int pollsLeft) {
            if (!Live(c)) return;
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(c.fid);
            if (!actor) { TeardownHand(c); return; }
            auto* hand = actor->GetMagicCaster(c.left ? CS::kLeftHand : CS::kRightHand);
            if (!hand) { TeardownHand(c); return; }

            const auto stNum = static_cast<std::uint32_t>(hand->state.get());
            if (stNum == 0 || pollsLeft <= 0) {   // channel ended on its own, or our hold window is up
                hand->InterruptCast(false);       // stop the still-active engine channel (no refund)
                actor->NotifyAnimationGraph("InterruptCast");
                actor->NotifyAnimationGraph("CastStop");
                spdlog::info("[ch.8+act] 0x{} concentration hold ended ({} hand, state {}).",
                             apmf::log::Hex(c.fid), c.left ? "left" : "right", stNum);
                ParkHand(c);
                return;
            }
            apmf::mainthread::Post([c, pollsLeft] { PhaseHold(c, pollsLeft - 1); });
        }

        // Phase 2: poll the caster until it reaches Charged (deck-observed >=3),
        // fire the graph RELEASE event once, then either PARK (instant spells --
        // never SUSTAINS past one pulse, a repeat Repoint fires again from rest,
        // #0/#1a rule 3's bounded-one-shot shape) or hand off to PhaseHold
        // (concentration spells -- see PhaseHold's header).
        void PhaseFire(DriveCtx c, int pollsLeft) {
            if (!Live(c)) return;   // cancelled/superseded -- nothing to do
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(c.fid);
            if (!actor) { TeardownHand(c); return; }
            auto* hand = actor->GetMagicCaster(c.left ? CS::kLeftHand : CS::kRightHand);
            if (!hand) { FireFallback(c); TeardownHand(c); return; }

            const auto stNum = static_cast<std::uint32_t>(hand->state.get());
            if (stNum >= 3) {   // Charged/Casting -- release
                actor->NotifyAnimationGraph(c.left ? "MLh_SpellFire_Event" : "MRh_SpellFire_Event");
                actor->NotifyAnimationGraph(c.left ? "MLh_WinStart"        : "MRh_WinStart");
                spdlog::info("[ch.8+act] 0x{} SpellFire ({} hand, state {}, form 0x{}).",
                             apmf::log::Hex(c.fid), c.left ? "left" : "right", stNum,
                             apmf::log::Hex(c.castForm));
                auto*      origSpell    = RE::TESForm::LookupByID<RE::SpellItem>(c.spell);
                const bool concentration = origSpell &&
                    origSpell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
                if (concentration) {
                    apmf::mainthread::Post([c] { PhaseHold(c, kConcentrationHoldPolls); });
                } else {
                    apmf::mainthread::Post([c] { ParkHand(c); });
                }
                return;
            }
            if (stNum == 0) {   // never entered charge -- degrade
                spdlog::info("[ch.8+act] 0x{} caster at rest before charge -- degrading to fallback.",
                             apmf::log::Hex(c.fid));
                FireFallback(c);
                TeardownHand(c);
                return;
            }
            if (pollsLeft <= 0) {   // wedged mid-charge -- degrade
                spdlog::info("[ch.8+act] 0x{} WEDGED at state {} -- degrading to fallback.",
                             apmf::log::Hex(c.fid), stNum);
                FireFallback(c);
                TeardownHand(c);
                return;
            }
            apmf::mainthread::Post([c, pollsLeft] { PhaseFire(c, pollsLeft - 1); });
        }

        // Phase 1: wait for the (queued) equip to actually SELECT the driven form,
        // then start the request from rest with the BeginCast anim +
        // RequestCastImpl, and hand to PhaseFire.
        void PhaseSelect(DriveCtx c, int triesLeft) {
            if (!Live(c)) return;
            auto* actor    = RE::TESForm::LookupByID<RE::Actor>(c.fid);
            auto* castForm = RE::TESForm::LookupByID<RE::SpellItem>(c.castForm);
            auto* target   = c.target ? RE::TESForm::LookupByID<RE::Actor>(c.target) : nullptr;
            if (!actor || !castForm) { TeardownHand(c); return; }
            auto* hand = actor->GetMagicCaster(c.left ? CS::kLeftHand : CS::kRightHand);
            if (!hand) { FireFallback(c); TeardownHand(c); return; }

            if (hand->currentSpell != castForm) {   // equip queued -- not selected yet
                if (triesLeft <= 0) {
                    spdlog::info("[ch.8+act] 0x{} never selected the drive form 0x{} -- degrading.",
                                 apmf::log::Hex(c.fid), apmf::log::Hex(c.castForm));
                    FireFallback(c);
                    TeardownHand(c);
                    return;
                }
                if (auto* mgr = RE::ActorEquipManager::GetSingleton())
                    mgr->EquipSpell(actor, castForm, HandSlot(c.left));
                actor->DrawWeaponMagicHands(true);
                apmf::mainthread::Post([c, triesLeft] { PhaseSelect(c, triesLeft - 1); });
                return;
            }

            // Selected. Drive ONLY from rest (re-requesting mid-sequence wedges the
            // caster in charge-glow -- the field-proven discipline this replicates).
            if (hand->state.get() != RE::MagicCaster::State::kNone)
                hand->InterruptCast(true);
            if (target) hand->desiredTarget = target->CreateRefHandle();

            float                              strength = 1.0f;
            RE::MagicSystem::CannotCastReason  reason{};
            hand->CheckCast(castForm, false, &strength, &reason, false);   // engine's own gate; logged only

            actor->NotifyAnimationGraph(c.left ? "BeginCastLeft" : "BeginCastRight");
            hand->RequestCastImpl();

            apmf::mainthread::Post([c] { PhaseFire(c, kChargePolls); });
        }

        // Start (or restart) a single hand's drive: resolve the target, decide the
        // delivery-flip proxy, claim internal ch.8b protection for this hand, equip,
        // and kick PhaseSelect. Tears down any PRIOR drive on this same hand first.
        void StartHandDrive(RE::FormID id, RE::Actor* actor, RE::SpellItem* spell, bool left,
                            RE::FormID explicitTarget) {
            ActorDrive& ad = g_drives[id];
            HandDrive&  hd = left ? ad.hand[1] : ad.hand[0];

            if (hd.active) {
                DriveCtx old{ id, hd.spell, hd.castForm, hd.target, left, hd.epoch };
                TeardownHand(old);
            }

            const RE::FormID targetFid    = ResolveTarget(actor, explicitTarget);
            const bool       selfDelivery = spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
            const bool       mismatch     = selfDelivery && targetFid != id;

            RE::SpellItem* castForm = spell;
            RE::FormID     proxyFid = 0;
            if (mismatch) {
                if (auto* px = proxy::Acquire(id, spell)) { castForm = px; proxyFid = px->GetFormID(); }
                // else: pool overflow -- drive the original (logged; self-pose but never a crash)
            }

            hd.active   = true;
            hd.inFlight = true;
            hd.spell    = spell->GetFormID();
            hd.castForm = castForm->GetFormID();
            hd.target   = (targetFid == id) ? 0 : targetFid;
            hd.epoch    = g_epoch.fetch_add(1, std::memory_order_relaxed);

            const bool concentration =
                spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
            APMF_API::APMF_CastRequest req{};
            req.spell  = hd.spell;
            req.proxy  = proxyFid;
            req.target = hd.target;
            req.flags  = (left ? APMF_API::kCastFlag_LeftHand : 0u) |
                         (concentration ? APMF_API::kCastFlag_Concentration : 0u);
            req.ttlMs  = APMF_API::kCastMaxTtlMs;   // safety-net ceiling; the drive tears down
                                                     // well before this in every normal path
            hd.claim = apmf::ControlMap::Get().EnqueueCast(id, kDriveBasis, &req);

            if (auto* mgr = RE::ActorEquipManager::GetSingleton())
                mgr->EquipSpell(actor, castForm, HandSlot(left));
            actor->DrawWeaponMagicHands(true);

            spdlog::info("[ch.8+act] 0x{} driving {} hand -- spell 0x{} cast-as 0x{} target 0x{}.",
                         apmf::log::Hex(id), left ? "left" : "right", apmf::log::Hex(hd.spell),
                         apmf::log::Hex(hd.castForm), apmf::log::Hex(hd.target));

            DriveCtx c{ id, hd.spell, hd.castForm, hd.target, left, hd.epoch };
            PhaseSelect(c, kSelectTries);
        }

        // 0 auto (prefers a free hand, no weapon there) / 1 right / 2 left / 3 dual.
        void ResolveHands(RE::Actor* actor, std::int32_t mode, bool& wantRight, bool& wantLeft) {
            wantRight = wantLeft = false;
            switch (mode) {
            case kHandRight: wantRight = true; return;
            case kHandLeft:  wantLeft  = true; return;
            case kHandDual:  wantRight = wantLeft = true; return;
            default: break;   // kHandAuto and any unrecognized value
            }
            auto* rightObj = actor->GetEquippedObject(false);
            auto* leftObj  = actor->GetEquippedObject(true);
            const bool rightHasWeapon = rightObj && rightObj->As<RE::TESObjectWEAP>();
            const bool leftHasWeapon  = leftObj  && leftObj->As<RE::TESObjectWEAP>();
            if (!rightHasWeapon)     wantRight = true;
            else if (!leftHasWeapon) wantLeft  = true;
            else                     wantRight = true;   // both occupied -- bump the right hand
        }

        void ApplyDesired(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) {
            // param.posX/Y/Z (a world-location target, e.g. for a Rune/AoE
            // ground-target cast) is ACCEPTED but NOT YET WIRED here -- reserved
            // for a later location-aim pass (see APMF_API.h's APMF_Param comment).
            // param.target IS wired below (ResolveTarget) -- the actor-target fix
            // this pass exists for (heal-the-player).
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(param.form);
            if (!spell) {
                spdlog::warn("[ch.8+act] 0x{} claimed with no loadable SpellItem (form 0x{}) -- ignored.",
                             apmf::log::Hex(id), apmf::log::Hex(param.form));
                return;
            }

            if (REL::Module::IsVR()) {
                // No per-hand deny exists on VR (CastGate/EquipGate/ActionGate are
                // all VR-refused, feat/deny-perhand) -- driving an equip+animate
                // sequence would race the native AI with no protection. Degrade
                // straight to the guaranteed fallback (marth's rule #5); no
                // hand-drive state is recorded (nothing to hold/restore).
                const RE::FormID targetFid = ResolveTarget(actor, param.target);
                DriveCtx c{ id, spell->GetFormID(), spell->GetFormID(),
                            (targetFid == id) ? 0 : targetFid, false, 0 };
                FireFallback(c);
                return;
            }

            bool wantRight = false, wantLeft = false;
            ResolveHands(actor, param.ival & kHandModeMask, wantRight, wantLeft);   // mask off kActFlag_Drive

            const RE::FormID spellFid = spell->GetFormID();
            ActorDrive&      ad       = g_drives[id];

            // A hand no longer wanted (e.g. dual -> right-only) is torn down
            // regardless of in-flight state -- the client changed its mind.
            // A hand that's still wanted, still mid-cast (inFlight), AND being
            // asked for the SAME spell is left alone (let it finish -- a
            // fast-repeating gambit tick must not keep restarting the same cast
            // before it ever fires). Anything else (not active, PARKED after a
            // prior fire, or a genuinely different spell/hand) restarts fresh --
            // this is what makes an identical repeat Repoint mean "fire again."
            auto apply = [&](bool left, bool want) {
                HandDrive& hd = left ? ad.hand[1] : ad.hand[0];
                if (!want) {
                    if (hd.active) TeardownHand(DriveCtx{ id, hd.spell, hd.castForm, hd.target, left, hd.epoch });
                    return;
                }
                if (hd.active && hd.inFlight && hd.spell == spellFid) return;   // still mid-cast -- don't restart
                StartHandDrive(id, actor, spell, left, param.target);
            };
            apply(false, wantRight);
            apply(true, wantLeft);
        }

    }

    void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) {
        if (!actor) return;
        ApplyDesired(id, actor, param);
    }

    void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) {
        if (!actor) return;
        ApplyDesired(id, actor, param);
    }

    void Release(RE::FormID id, RE::Actor* /*actor*/) {
        auto it = g_drives.find(id);
        if (it == g_drives.end()) return;
        for (bool left : { false, true }) {
            HandDrive& hd = left ? it->second.hand[1] : it->second.hand[0];
            if (!hd.active) continue;
            TeardownHand(DriveCtx{ id, hd.spell, hd.castForm, hd.target, left, hd.epoch });
        }
        g_drives.erase(it);   // any stale posted continuation now finds no entry -> no-op
    }

}
