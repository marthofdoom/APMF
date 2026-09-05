#include "PCH.h"
#include "core/Log.h"
#include "core/CastExecutor.h"
#include "core/ControlMap.h"
#include "core/MainThread.h"
#include "core/Clock.h"

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

        // ---- WALL-CLOCK BUDGETS, NOT FRAME COUNTS (H4/H5, 2026-09-05 review) ----
        //
        // Every one of these was a POLL COUNT (kSelectPolls=300, kChargePolls=300,
        // kConcentrationHoldPolls=180). MainThread::Pump runs exactly ONE poll per
        // frame, so "300 polls" is "~5s" only at exactly 60fps: on a 144Hz machine
        // the charge budget collapsed to 2.1s against a MEASURED ~5s engine (the
        // AI's own organic cast takes ~5s claim->Charged), so the drive degraded to
        // the instant fallback on every pulse -- a frame-rate-dependent failure of
        // the exact "read the engine before it updated" class the last three field
        // cycles were spent on. They are now deadlines on the SAME monotonic clock
        // the ch.8b claim TTL itself uses (core/Clock.h, apmf::clock::MonotonicMs),
        // so a fast machine and a slow one wait the same real seconds.
        //
        // TTL SAFETY (H5): the internal ch.8b protection claim is bounded by
        // APMF_API::kCastMaxTtlMs (15000, a FROZEN ABI constant -- it cannot be
        // raised). The per-phase budgets below sum to more than that in the worst
        // case, so EVERY phase deadline is additionally clamped to ONE whole-drive
        // deadline (kDriveTotalMs, set once in StartHandDrive) that sits comfortably
        // under the TTL. That makes it structurally impossible for the drive to
        // still be running -- unprotected -- after its own claim has expired.
        constexpr std::uint64_t kSelectWaitMs = 5000;    // wait for the ONE queued EquipSpell to land
        constexpr std::uint64_t kRestWaitMs   = 1500;    // wait for the caster to actually return to rest
        constexpr std::uint64_t kChargeWaitMs = 5000;    // wait for kNone -> ... -> Charged (measured ~5s)
        constexpr std::uint64_t kConcHoldMs   = 3000;    // how long a concentration spell CHANNELS after
                                                          // firing, so it applies over a real window rather
                                                          // than one instantaneous pulse (2026-09-05 fix)
        constexpr std::uint64_t kParkGraceMs  = 1500;    // let the RELEASE animation play out before the
                                                          // stop tail (H7) -- ends early the moment the
                                                          // engine concludes the cast on its own
        constexpr std::uint64_t kLogEveryMs   = 500;     // periodic diagnostic cadence, spam-free
        constexpr std::uint64_t kDriveTotalMs = 12000;   // whole-drive ceiling; MUST stay comfortably
                                                          // below APMF_API::kCastMaxTtlMs (15000)
        static_assert(kDriveTotalMs + 2000 <= APMF_API::kCastMaxTtlMs,
                      "the whole-drive budget must finish well inside the ch.8b claim TTL -- a drive "
                      "that outlives its own claim runs UNPROTECTED (H5)");

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
                    if (!actor->HasSpell(s.form))
                        actor->AddSpell(s.form);   // TRANSIENT teach -- selectable (removed in Free/Unteach)
                    return s.form;
                }
                spdlog::warn("[ch.8+act] proxy pool overflow (owner 0x{}, all {} slots held by "
                             "other live streams) -- caller must decline/fallback, never drive "
                             "the original self-delivery form (2026-09-06 correctness fix).",
                             apmf::log::Hex(a_owner), sizeof(g_slot) / sizeof(g_slot[0]));
                return nullptr;
            }
            // Un-teach WITHOUT releasing the slot (H3). A hand that has already
            // FIRED and is merely PARKED must not leave a runtime 0xFF dynamic
            // form in the actor's known-spell list, because a quicksave taken in
            // that window would persist a reference to a form that does not exist
            // on the next load. The slot stays OWNED (this drive still owns the
            // hand and will tear it down properly); only the actor's knowledge of
            // the transient is dropped. `Acquire` re-teaches on the next drive
            // (its `!HasSpell` guard).
            void Unteach(RE::FormID a_owner) {
                for (auto& s : g_slot) if (s.owner == a_owner && s.form) {
                    if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_owner))
                        actor->RemoveSpell(s.form);
                }
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
            // Un-teach + deselect EVERY live proxy, keeping the slots. Pre-save
            // sweep (H3): belt-and-braces so no quicksave taken at any point in
            // the drive can capture a transient 0xFF spell, whatever phase the
            // chain happens to be in. A drive whose proxy is pulled out from under
            // it simply fails its equip/charge check and degrades to the
            // guaranteed-delivery fallback -- the effect still lands.
            void UnteachAll() {
                for (auto& s : g_slot) {
                    if (!s.owner || !s.form) continue;
                    if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(s.owner)) {
                        actor->DeselectSpell(s.form);
                        actor->RemoveSpell(s.form);
                    }
                }
            }
            // Revert / kPreLoadGame reset. The IFormFactory-minted 0xFF dynamic
            // forms do NOT survive a load, so null every slot so the next cast
            // re-mints -- and clear each form's BORROWED source `Effect*` FIRST
            // (Configure copied them BY POINTER, sharing the source spell's own
            // effect objects) so the load-time form purge frees an EMPTY array and
            // can never double-free a live source spell's effects. This is MFO's
            // own hard-won lesson, verbatim in shape: MFO
            // `native/Actuation_Direct.cpp` ConcProxy::Reset. Without it, the pool
            // ALSO stayed permanently occupied across a revert (M8) -- the 4 slots
            // kept a stale `owner`, so ally heals silently stopped for the rest of
            // the session. Main thread (the SKSE revert/pre-load seat).
            void Reset() {
                for (auto& s : g_slot) {
                    if (s.form) s.form->effects.clear();   // drop borrowed source Effect*
                    s = {};
                }
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
            // Monotonic deadline for the WHOLE drive (H5). Set once in
            // StartHandDrive to now + kDriveTotalMs; every per-phase deadline is
            // clamped to it, so the chain can never still be running after its own
            // ch.8b protection claim's TTL has lapsed. 0 == "no clamp" and is used
            // only by the teardown-only contexts (Release/TeardownHand/the overflow
            // decline), which never post a phase.
            std::uint64_t driveDeadlineMs = 0;
        };

        // A phase's own wall-clock budget: when to give up, and when to next emit a
        // periodic diagnostic. Passed BY VALUE through the phase chain exactly like
        // the old `pollsLeft` int was, so a re-post carries its own state and no
        // shared mutable timer exists.
        struct Budget {
            std::uint64_t deadlineMs = 0;
            std::uint64_t nextLogMs  = 0;
        };

        // Start a phase budget of `ms`, clamped to the whole-drive deadline (H5).
        Budget MakeBudget(const DriveCtx& c, std::uint64_t ms) {
            const auto now = apmf::clock::MonotonicMs();
            std::uint64_t deadline = now + ms;
            if (c.driveDeadlineMs != 0 && deadline > c.driveDeadlineMs)
                deadline = c.driveDeadlineMs;   // never outlive the ch.8b claim
            return Budget{ deadline, now + kLogEveryMs };
        }
        bool Expired(const Budget& b) { return apmf::clock::MonotonicMs() >= b.deadlineMs; }
        // True at most once per kLogEveryMs; advances the budget's own log cursor.
        bool LogDue(Budget& b) {
            const auto now = apmf::clock::MonotonicMs();
            if (now < b.nextLogMs) return false;
            b.nextLogMs = now + kLogEveryMs;
            return true;
        }

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

        // Diagnostic-only (2026-09-05 field fix, requirement 4): dumps the THREE
        // signals a select-wait cares about -- what's actually in the hand's
        // equip slot (the thing we're waiting to become `castForm`), the
        // caster's raw state, and MagicCaster::currentSpell (kept for
        // comparison even though it's no longer the gate) -- so a deck run
        // tells us definitively if this still doesn't land, rather than another
        // guess. Never gates anything; log-only.
        void LogHandDiagnostic(RE::Actor* a_actor, RE::MagicCaster* a_hand, bool a_left, const char* a_why) {
            auto* eq = a_actor->GetEquippedObject(a_left);
            spdlog::info("[ch.8+act] 0x{} {} hand diag ({}): equipped=0x{} ('{}') state={} currentSpell=0x{}",
                         apmf::log::Hex(a_actor->GetFormID()), a_left ? "left" : "right", a_why,
                         apmf::log::Hex(eq ? eq->GetFormID() : 0),
                         eq && eq->GetName() ? eq->GetName() : "none",
                         static_cast<std::uint32_t>(a_hand->state.get()),
                         apmf::log::Hex(a_hand->currentSpell ? a_hand->currentSpell->GetFormID() : 0));
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
        // Is the OTHER hand of this same actor still driving the SAME proxy form? A
        // kHandDual drive shares ONE proxy across both hands (the pool is keyed by
        // owner, not hand), so nothing may un-teach / deselect / free that form
        // while the sibling is still using it.
        bool SiblingUsesProxy(const DriveCtx& c) {
            const RE::FormID proxyFid = proxy::FormForOwner(c.fid);
            if (proxyFid == 0) return false;
            auto it = g_drives.find(c.fid);
            if (it == g_drives.end()) return false;
            const HandDrive& sibling = c.left ? it->second.hand[0] : it->second.hand[1];
            return sibling.active && sibling.castForm == proxyFid;
        }

        // The OBSERVED end-of-cast tail (deck capture 2026-09-04, step 5 of the
        // vanilla sequence: anim-event `InterruptCast` then `CastStop`), paired with
        // an actual caster stop.
        //
        // H7 (2026-09-05 review) -- ANIM EVENTS MUST BE BALANCED. Every
        // `BeginCastRight`/`MRh_WinStart` this file pushes has to be answered by the
        // matching stop, or the behaviour graph is left in a cast state it never
        // exits: a graph that never returned to idle can REJECT the next BeginCast
        // outright, which reads downstream as "the caster never left rest" -- i.e.
        // it manufactures exactly the symptom the last two field cycles chased.
        // Three unbalanced sites existed: PhaseSelect's pre-drive `InterruptCast(true)`
        // (caster stopped, graph never told), `ParkHand` (the non-concentration
        // SUCCESS path ended with NEITHER an interrupt nor a stop), and `MRh_WinStart`
        // (never balanced by any stop on any path). All three now route through here.
        //
        // Idempotent: the caster stop is skipped at rest and the graph events are
        // harmless no-ops on a graph already idle, so a path that reaches this twice
        // (e.g. PhaseHold -> ParkHand) costs nothing and cannot corrupt state.
        void EmitStopTail(RE::Actor* a_actor, bool a_left) {
            if (!a_actor) return;
            if (auto* hand = a_actor->GetMagicCaster(a_left ? CS::kLeftHand : CS::kRightHand)) {
                if (static_cast<std::uint32_t>(hand->state.get()) != 0)
                    hand->InterruptCast(false);   // stop the engine channel (no refund)
            }
            a_actor->NotifyAnimationGraph("InterruptCast");
            a_actor->NotifyAnimationGraph("CastStop");
        }

        void TeardownHand(const DriveCtx& c) {
            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(c.fid)) {
                EmitStopTail(actor, c.left);   // H7 -- balanced caster stop + graph tail
                if (auto* px = RE::TESForm::LookupByID<RE::SpellItem>(c.castForm))
                    actor->DeselectSpell(px);   // no lingering equipped spell (light-limit CTD lesson)
            }
            if (HandDrive* hd = Live(c)) {
                apmf::ControlMap::Get().EnqueueRelease(hd->claim);
                hd->claim  = APMF_API::kInvalidHandle;
                hd->active = false;
            }
            if (!SiblingUsesProxy(c)) proxy::Free(c.fid);
        }

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
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(c.fid);

            // H7: the success path used to end with NEITHER an interrupt NOR a stop
            // event, leaving `BeginCast*`/`MRh_WinStart` permanently unanswered in
            // the behaviour graph. Every park now closes the sequence properly.
            EmitStopTail(actor, c.left);

            // H3: a PARKED hand must never leave the runtime 0xFF delivery-flip
            // PROXY equipped or known. The proxy is minted by IFormFactory and
            // AddSpell'd transiently; a quicksave taken while a hand sat parked
            // would persist a reference to a dynamic form that does not exist on the
            // next load. So the proxy is un-taught + deselected the moment the cast
            // is done with it -- unless the sibling hand is still driving that same
            // form. The slot stays OWNED (this drive still owns the hand and tears
            // it down properly later); `proxy::Acquire` re-teaches on the next drive.
            //
            // Scoped to the PROXY case (castForm != spell) on purpose: a REAL spell
            // left equipped is not a save hazard, and deselecting it here would cost
            // the parked hand its fast re-fire (a repeat Repoint would have to pay
            // the whole multi-second re-equip wait again) for no correctness gain.
            if (actor && c.castForm != c.spell && !SiblingUsesProxy(c)) {
                if (auto* px = RE::TESForm::LookupByID<RE::SpellItem>(c.castForm))
                    actor->DeselectSpell(px);
                proxy::Unteach(c.fid);
            }

            if (HandDrive* hd = Live(c)) {
                apmf::ControlMap::Get().EnqueueRelease(hd->claim);
                hd->claim    = APMF_API::kInvalidHandle;
                hd->inFlight = false;   // parked -- a same-spell Repoint now fires again
            }
        }

        // Phase 3a (non-concentration success tail). The SpellFire event has just
        // been pushed; give the RELEASE animation a bounded moment to play and the
        // engine a chance to conclude the cast on its OWN (state -> 0) before the
        // stop tail lands, so H7's balancing does not visually clip the very
        // animation this whole drive exists to produce. Ends the instant the caster
        // reaches rest, so a normal cast pays no added latency.
        //
        // M7 (diagnostic ONLY -- changes no control flow): the success path never
        // verified that anything was actually delivered; an animated cast that
        // applies nothing was indistinguishable from a real one in the log. The
        // first poll here dumps the caster state one frame after the fire, which is
        // what tells a deck run "it fired and concluded" apart from "the fire event
        // was accepted and nothing happened".
        void PhaseParkWait(DriveCtx c, Budget b, bool firstPoll) {
            if (!Live(c)) return;
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(c.fid);
            if (!actor) { TeardownHand(c); return; }
            auto* hand = actor->GetMagicCaster(c.left ? CS::kLeftHand : CS::kRightHand);
            if (!hand) { ParkHand(c); return; }

            if (firstPoll)
                LogHandDiagnostic(actor, hand, c.left, "post-fire (one frame after SpellFire) -- M7 probe");

            const auto stNum = static_cast<std::uint32_t>(hand->state.get());
            if (stNum == 0 || Expired(b)) {
                spdlog::info("[ch.8+act] 0x{} cast released and {} ({} hand, state {}) -- parking.",
                             apmf::log::Hex(c.fid),
                             stNum == 0 ? "returned to rest" : "still busy at the park grace deadline",
                             c.left ? "left" : "right", stNum);
                ParkHand(c);
                return;
            }
            apmf::mainthread::Post([c, b] { PhaseParkWait(c, b, false); });
        }

        // Phase 3 (concentration only, 2026-09-05 field fix): after the initial
        // SpellFire, a concentration spell's magnitude is applied by the ENGINE's
        // OWN per-frame caster update for as long as it stays in the charged/
        // casting state (>=3) -- exactly like a player holding the cast button.
        // So we do NOT interrupt/re-fire here, just keep the channel (and the
        // internal ch.8b claim protecting the hand) alive by polling, up to a
        // bounded hold window (kConcHoldMs, wall clock) or until the caster exits
        // the state on its own -- whichever comes first -- then parks (single
        // instant-apply fallback semantics don't apply here: the spell already
        // fired at least once, so there is nothing to degrade to).
        //
        // M1 (2026-09-05 review): this used to end the channel on a SINGLE
        // `stNum == 0` read -- the exact transient-as-terminal shape PhaseFire had
        // just been fixed for. The state can still be spinning up in the frames
        // right after SpellFire, and reading a 0 there ended the heal instantly.
        // `everActive` mirrors PhaseFire's `everLeftRest`: until this chain has seen
        // the caster busy at least once, a 0 means "not spun up yet" and we keep
        // waiting; only a 0 AFTER a busy observation is a genuine channel end.
        //
        // M2 (target half): the drive set `desiredTarget` ONCE, at BeginCast time,
        // so the AI was free to re-aim a channelled heal mid-cast (onto its combat
        // foe, in the worst case). It is now re-asserted on every hold poll. NOTE:
        // whether a concentration channel ALSO needs a per-tick RE-FIRE to keep
        // applying is RE-GATED and deliberately NOT guessed here -- the engine's own
        // per-frame caster update is believed to apply the magnitude for as long as
        // the state holds, and nothing measured contradicts that yet.
        void PhaseHold(DriveCtx c, Budget b, bool everActive) {
            if (!Live(c)) return;
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(c.fid);
            if (!actor) { TeardownHand(c); return; }
            auto* hand = actor->GetMagicCaster(c.left ? CS::kLeftHand : CS::kRightHand);
            if (!hand) { TeardownHand(c); return; }

            const auto stNum = static_cast<std::uint32_t>(hand->state.get());
            if (stNum > 0) everActive = true;   // the channel is genuinely running

            // M2: hold the aim for the WHOLE channel, not just at BeginCast.
            if (c.target != 0) {
                if (auto* tgt = RE::TESForm::LookupByID<RE::Actor>(c.target))
                    hand->desiredTarget = tgt->CreateRefHandle();
            }

            const bool channelEnded = (stNum == 0 && everActive);
            if (channelEnded || Expired(b)) {
                spdlog::info("[ch.8+act] 0x{} concentration hold ended ({} hand, state {}, {}).",
                             apmf::log::Hex(c.fid), c.left ? "left" : "right", stNum,
                             channelEnded ? "channel closed" : "hold window elapsed");
                ParkHand(c);   // ParkHand emits the balanced stop tail (H7)
                return;
            }
            if (LogDue(b))
                LogHandDiagnostic(actor, hand, c.left, everActive ? "channelling" : "waiting-to-channel");
            apmf::mainthread::Post([c, b, everActive] { PhaseHold(c, b, everActive); });
        }

        // Phase 2: poll the caster until it reaches Charged (deck-observed >=3),
        // fire the graph RELEASE event once, then either PARK (instant spells --
        // never SUSTAINS past one pulse, a repeat Repoint fires again from rest,
        // #0/#1a rule 3's bounded-one-shot shape) or hand off to PhaseHold
        // (concentration spells -- see PhaseHold's header).
        //
        // STATE-0 IS NOT TERMINAL ON ITS OWN (2026-09-06 field fix, the SAME
        // class of bug the select-signal fix corrected). `state==0` right after
        // RequestCastImpl means "has not spun up YET" -- the deck showed
        // RequestCastImpl fired and the VERY NEXT poll (same millisecond) still
        // read state 0, and the old code treated that as a terminal refusal,
        // degrading on literally every pulse. The observed vanilla walk is
        // 0 -> 1(ready) -> 2(Charging) -> 3(Charged) -> 4(Casting), and the
        // engine can take seconds to advance it (the AI's own organic cast took
        // ~5s claim->Charged). So `everLeftRest` tracks whether THIS poll chain
        // has EVER observed state>0: while it hasn't, state==0 is "still waiting
        // to spin up" and we keep polling (bounded by kChargeWaitMs, same scale
        // as the select wait). Once it HAS spun up at least once and THEN
        // returns to state==0 without ever reaching >=3, that is a genuine
        // early end/refusal -- terminal immediately, not after the whole
        // remaining budget (further polling would just re-observe the same
        // rest). Wedged mid-charge (stuck at 1/2 the whole budget) still
        // degrades once the budget elapses, as before.
        //
        // M3 (2026-09-05 review) -- DO NOT DOUBLE-APPLY. The "returned to rest
        // without charging" branch is only safe if the cast really never happened.
        // A FAST cast can walk 1 -> 2 -> 3 -> 4 -> 6 -> 0 entirely BETWEEN two
        // polls, and the old code then saw a 0 with `everLeftRest` set, called it a
        // refusal, and fired the instant fallback AFTER the animated cast had
        // already landed -- a silent double heal. `everCharging` records whether the
        // chain ever observed state>=2 (Charging or beyond, i.e. the cast was
        // genuinely under way): if it did, a subsequent rest is a COMPLETED cast, so
        // we tear down quietly and never fire the fallback. Only a chain that never
        // got past state 1 (ready) still degrades on that path.
        void PhaseFire(DriveCtx c, Budget b, bool everLeftRest = false, bool everCharging = false) {
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
                    const Budget hb = MakeBudget(c, kConcHoldMs);
                    apmf::mainthread::Post([c, hb] { PhaseHold(c, hb, false); });
                } else {
                    const Budget pb = MakeBudget(c, kParkGraceMs);
                    apmf::mainthread::Post([c, pb] { PhaseParkWait(c, pb, true); });
                }
                return;
            }

            if (stNum > 0) everLeftRest = true;   // spun up into ready/charging at least once
            if (stNum >= 2) everCharging = true;  // M3 -- the cast was genuinely under way

            if (LogDue(b))
                LogHandDiagnostic(actor, hand, c.left, everLeftRest ? "charging" : "waiting-to-leave-rest");

            if (stNum == 0 && everLeftRest) {
                if (everCharging) {
                    // M3: it was CHARGING (>=2) and is now at rest -- the whole
                    // 2->3->4->6->0 walk completed between two polls. The animated
                    // cast already landed; firing the fallback here would apply the
                    // effect a SECOND time. Tear down quietly, no fallback.
                    LogHandDiagnostic(actor, hand, c.left,
                                      "charged-then-rest between polls -- cast completed, NOT degrading");
                    TeardownHand(c);
                    return;
                }
                // It reached ready (1) and returned to rest WITHOUT ever charging --
                // a real early end/refusal, not a startup delay. Terminal now
                // (waiting out the rest of the budget would not change anything).
                LogHandDiagnostic(actor, hand, c.left, "returned-to-rest-without-charging -- degrading");
                FireFallback(c);
                TeardownHand(c);
                return;
            }
            if (Expired(b)) {   // whole wait budget elapsed -- never left rest, or wedged mid-charge
                LogHandDiagnostic(actor, hand, c.left,
                                  everLeftRest ? "WEDGED mid-charge -- degrading"
                                               : "never left rest -- degrading");
                FireFallback(c);
                TeardownHand(c);
                return;
            }
            apmf::mainthread::Post([c, b, everLeftRest, everCharging] {
                PhaseFire(c, b, everLeftRest, everCharging);
            });
        }

        // Phase 1b (H6, 2026-09-05 review): DRIVE ONLY FROM ACTUAL REST.
        //
        // PhaseSelect used to interrupt the caster and call RequestCastImpl in the
        // SAME frame -- directly against its own comment ("Drive ONLY from rest;
        // re-requesting mid-sequence wedges the caster in charge-glow"). An
        // InterruptCast is a REQUEST to the engine, not an instantaneous state
        // change: reading/driving the caster in the same frame is the identical
        // "read the engine before it updated it" mistake that cost the previous
        // three field cycles. So the interrupt now happens at the end of
        // PhaseSelect (paired with its anim-graph stop tail, H7) and THIS phase
        // polls, on the wall clock, until the caster is genuinely at kNone before
        // pushing BeginCast + RequestCastImpl.
        //
        // If it never reaches rest inside kRestWaitMs we degrade rather than drive
        // from a busy caster: the guaranteed-delivery fallback still lands the
        // effect, and driving anyway is the wedge this phase exists to prevent.
        void PhaseRest(DriveCtx c, Budget b) {
            if (!Live(c)) return;
            auto* actor    = RE::TESForm::LookupByID<RE::Actor>(c.fid);
            auto* castForm = RE::TESForm::LookupByID<RE::SpellItem>(c.castForm);
            if (!actor || !castForm) { TeardownHand(c); return; }
            auto* hand = actor->GetMagicCaster(c.left ? CS::kLeftHand : CS::kRightHand);
            if (!hand) { FireFallback(c); TeardownHand(c); return; }

            if (static_cast<std::uint32_t>(hand->state.get()) != 0) {   // not at rest yet
                if (Expired(b)) {
                    LogHandDiagnostic(actor, hand, c.left,
                                      "never returned to rest after the interrupt -- degrading");
                    FireFallback(c);
                    TeardownHand(c);
                    return;
                }
                if (LogDue(b)) LogHandDiagnostic(actor, hand, c.left, "waiting-for-rest");
                apmf::mainthread::Post([c, b] { PhaseRest(c, b); });
                return;
            }

            // At rest, spell in hand -- start the request.
            if (c.target != 0) {
                if (auto* tgt = RE::TESForm::LookupByID<RE::Actor>(c.target))
                    hand->desiredTarget = tgt->CreateRefHandle();
            }

            float                             strength = 1.0f;
            RE::MagicSystem::CannotCastReason reason{};
            const bool engineWillCast =
                hand->CheckCast(castForm, false, &strength, &reason, false);   // engine's own gate
            // Logged, never gated on (the drive requests regardless -- the engine
            // gets the final word inside RequestCastImpl). This line is the direct
            // read-out of the H1 fix: before it, THIS call was denied by APMF's own
            // CheckCast hook whenever castForm was the delivery-flip proxy.
            spdlog::info("[ch.8+act] 0x{} CheckCast(0x{}) -> {} (reason {}) -- diagnostic.",
                         apmf::log::Hex(c.fid), apmf::log::Hex(c.castForm),
                         engineWillCast ? "ALLOW" : "DENY", static_cast<std::uint32_t>(reason));

            actor->NotifyAnimationGraph(c.left ? "BeginCastLeft" : "BeginCastRight");
            hand->RequestCastImpl();

            const Budget fb = MakeBudget(c, kChargeWaitMs);
            apmf::mainthread::Post([c, fb] { PhaseFire(c, fb); });
        }

        // Phase 1: wait for the ALREADY-QUEUED equip (issued once, in
        // StartHandDrive) to land, then interrupt to rest (PhaseRest) before the
        // BeginCast anim + RequestCastImpl.
        //
        // SELECT SIGNAL (2026-09-05 field fix, replacing a 100% degrade). This
        // used to gate on `MagicCaster::currentSpell == castForm`. Deck evidence
        // (APMF.log, both hands, proxied AND un-proxied spells alike) showed it
        // NEVER becomes true within any pre-cast window: `currentSpell` is
        // populated only once the caster STARTS CASTING (state charging/
        // charged/casting), not merely once a spell is equipped-and-ready --
        // core/CastObserve.h's own passive observer confirms this independently
        // (every logged line carrying a spell is at state>=2; state==1(ready)
        // NEVER carries one). Gating a PRE-cast check on a field that is only
        // set BY casting is a logic inversion that can never pass -- exactly the
        // reported 100% degrade. The correct signal for "did the equip land" is
        // the ACTOR's equipped object for that hand (MFO's own established
        // pattern, e.g. its Loadout.cpp/Actuation.cpp: "selection comes from the
        // equip, NOT currentSpell").
        void PhaseSelect(DriveCtx c, Budget b) {
            if (!Live(c)) return;
            auto* actor    = RE::TESForm::LookupByID<RE::Actor>(c.fid);
            auto* castForm = RE::TESForm::LookupByID<RE::SpellItem>(c.castForm);
            if (!actor || !castForm) { TeardownHand(c); return; }
            auto* hand = actor->GetMagicCaster(c.left ? CS::kLeftHand : CS::kRightHand);
            if (!hand) { FireFallback(c); TeardownHand(c); return; }

            if (actor->GetEquippedObject(c.left) != castForm) {   // the equip hasn't landed yet
                if (Expired(b)) {
                    LogHandDiagnostic(actor, hand, c.left, "never-equipped -- degrading");
                    FireFallback(c);
                    TeardownHand(c);
                    return;
                }
                if (LogDue(b)) LogHandDiagnostic(actor, hand, c.left, "waiting-for-equip");
                // Do NOT re-issue EquipSpell here -- it was already queued ONCE
                // in StartHandDrive. Re-issuing every poll was the equip/
                // interrupt/re-equip churn a prior deck cycle logged (and, paired
                // with the AI's own context rebuild, tipped a since-fixed deny
                // gap into a CTD) -- just wait for the ONE queued equip to land.
                apmf::mainthread::Post([c, b] { PhaseSelect(c, b); });
                return;
            }

            LogHandDiagnostic(actor, hand, c.left, "equipped -- draining to rest before BeginCast");

            // Equipped. Drive ONLY from rest -- but the interrupt and the drive can
            // NOT share a frame (H6): ask the engine to stop here, PAIRED with the
            // anim-graph stop tail (H7 -- an unbalanced caster interrupt leaves the
            // behaviour graph in a cast state that can then REJECT our BeginCast),
            // and let PhaseRest confirm the caster actually reached rest before
            // anything is requested.
            if (static_cast<std::uint32_t>(hand->state.get()) != 0) {
                hand->InterruptCast(true);
                actor->NotifyAnimationGraph("InterruptCast");
                actor->NotifyAnimationGraph("CastStop");
            }

            const Budget rb = MakeBudget(c, kRestWaitMs);
            apmf::mainthread::Post([c, rb] { PhaseRest(c, rb); });
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

            const RE::FormID targetFid     = ResolveTarget(actor, explicitTarget);
            const bool       selfDelivery  = spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
            const bool       mismatch      = selfDelivery && targetFid != id;
            const bool       concentration =
                spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
            const RE::FormID hdTarget      = (targetFid == id) ? 0 : targetFid;

            RE::SpellItem* castForm = spell;
            RE::FormID     proxyFid = 0;
            if (mismatch) {
                if (auto* px = proxy::Acquire(id, spell)) {
                    castForm = px; proxyFid = px->GetFormID();
                } else {
                    // Proxy REQUIRED (self-delivery, non-self target) but the pool is
                    // exhausted (2026-09-06 correctness fix, marth-approved). Driving
                    // the ORIGINAL self-delivery form here would silently mis-deliver:
                    // for a CONCENTRATION spell this is PROVEN wrong -- MFO's own
                    // field-tested ConcProxy precedent (Actuation_Direct.cpp) found
                    // that CastSpellImmediate resolves a concentration kSelf channel
                    // onto the CASTER regardless of the target argument, and its own
                    // overflow policy is to SKIP entirely rather than cast the Self
                    // source off-slot. For a NON-concentration (FF/instant) spell,
                    // CastSpellImmediate targeting kSelf delivery is separately
                    // field-proven to land CORRECTLY on the passed target (MFO's own
                    // comment: "a fire-and-forget Self spell force-cast at another
                    // actor lands on that actor -- Candlelight/flesh work") -- so
                    // THAT case has a genuine guaranteed-delivery fallback even
                    // without a proxy; concentration does not. Either way we decline
                    // the ANIMATED drive here (no field evidence RequestCastImpl's
                    // driven channel targets kSelf delivery correctly at all, proxy
                    // or not) and go straight to the guaranteed-delivery decision.
                    DriveCtx fc{ id, spell->GetFormID(), spell->GetFormID(), hdTarget, left, 0 };
                    if (concentration) {
                        spdlog::warn("[ch.8+act] 0x{} proxy pool exhausted for a CONCENTRATION "
                                     "self-delivery cast at 0x{} -- DECLINING entirely (a plain "
                                     "apply would incorrectly land on the caster, not the target; "
                                     "will retry on the next Repoint once a proxy slot frees).",
                                     apmf::log::Hex(id), apmf::log::Hex(fc.target));
                    } else {
                        spdlog::warn("[ch.8+act] 0x{} proxy unavailable -- instant-apply at 0x{}, "
                                     "no animation (field-proven correct target for a "
                                     "non-concentration self-delivery spell).",
                                     apmf::log::Hex(id), apmf::log::Hex(fc.target));
                        FireFallback(fc);
                    }
                    return;   // this hand stays INACTIVE -- no claim, no equip, nothing to tear down
                }
            }

            hd.active   = true;
            hd.inFlight = true;
            hd.spell    = spell->GetFormID();
            hd.castForm = castForm->GetFormID();
            hd.target   = hdTarget;
            hd.epoch    = g_epoch.fetch_add(1, std::memory_order_relaxed);

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

            // ONE wall-clock ceiling for the WHOLE drive (H5), set here and clamped
            // into every phase budget: the internal ch.8b claim above lives at most
            // APMF_API::kCastMaxTtlMs, and a drive that outlived it would keep
            // equipping/animating with no protection at all -- the AI free to fight
            // it for the hand. kDriveTotalMs is static_assert'd to finish well inside
            // that TTL.
            DriveCtx c{ id, hd.spell, hd.castForm, hd.target, left, hd.epoch,
                        apmf::clock::MonotonicMs() + kDriveTotalMs };
            PhaseSelect(c, MakeBudget(c, kSelectWaitMs));
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
            // M6 (2026-09-05 review): "free hand" used to mean "no TESObjectWEAP
            // there", so a SHIELD (TESObjectARMO) or a TORCH (TESObjectLIGH) in the
            // left hand read as FREE and auto mode happily stripped it to cast --
            // taking a sword-and-board follower's shield off mid-fight. Anything the
            // engine considers held in that hand counts as occupied. A SPELL is NOT
            // occupied on purpose: replacing one spell with another is exactly what
            // this drive does.
            auto occupied = [](RE::TESForm* obj) {
                return obj && (obj->As<RE::TESObjectWEAP>() ||
                               obj->As<RE::TESObjectARMO>() ||   // shield
                               obj->As<RE::TESObjectLIGH>());    // torch
            };
            const bool rightHeld = occupied(actor->GetEquippedObject(false));
            const bool leftHeld  = occupied(actor->GetEquippedObject(true));
            if (!rightHeld)     wantRight = true;
            else if (!leftHeld) wantLeft  = true;
            else                wantRight = true;   // both occupied -- bump the right hand
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

    void ResetAll() {
        // Revert / new game / kPreLoadGame (H2 + M8). Two distinct leaks closed:
        //
        //  * The delivery-flip proxies are IFormFactory-minted 0xFF dynamic forms
        //    that do NOT survive a load, and `Configure` shares the SOURCE spell's
        //    `Effect*` objects BY POINTER. Left alone, the load-time form purge
        //    would free a live source spell's effect array through the dead proxy.
        //    `proxy::Reset` clears the borrowed pointers FIRST, then nulls the
        //    slots so the next cast re-mints -- MFO's own hard-won lesson
        //    (native/Actuation_Direct.cpp, ConcProxy::Reset), same shape.
        //  * `ControlMap::Clear()` (the revert path) deliberately does NOT call
        //    channel->Release, so nothing ever released the drive state: the 4-slot
        //    proxy pool stayed permanently occupied by a stale `owner` after a
        //    revert and every subsequent ally heal silently declined ("proxy pool
        //    overflow") for the rest of the session. Clearing g_drives here is what
        //    makes the revert path actually reset.
        //
        // No engine calls: on this path the actors are being replaced/torn down, so
        // this only drops APMF-side state. Any continuation still queued on the main
        // -thread pump finds no g_drives entry (Live() -> null) and no-ops. MAIN
        // THREAD ONLY (the SKSE revert / kPreLoadGame seat -- the same one Drain
        // runs on, per core/ControlMap.h).
        g_drives.clear();
        proxy::Reset();
    }

    void PreSaveSweep() {
        // H3, belt-and-braces. A drive that has fired and PARKED already un-taught
        // its proxy (see ParkHand), but a save can be taken at ANY point in the
        // chain -- including mid-charge, with the transient equipped and known. A
        // runtime 0xFF dynamic form must never be capturable into the .ess, so this
        // un-teaches + deselects every live proxy at save time. The slots stay
        // owned; a drive whose proxy is pulled out from under it simply fails its
        // equip/charge check and degrades to the guaranteed-delivery fallback, so
        // the effect still lands. MAIN THREAD ONLY (SKSE's save callback seat).
        proxy::UnteachAll();
    }

}
