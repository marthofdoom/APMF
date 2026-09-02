#include "PCH.h"

// ============================================================================
// APMF — AI Package Management Framework.  PROBE / GAPS build  (branch probe/gaps).
//
// THROWAWAY OBSERVATION build. NOT the real architecture. Its only job is to
// resolve the empirical GAPS in Docs/CHANNEL-MAP.md by driving/observing each
// gap channel under a hotkey with heavy tagged logging, on a live 1.6.1170
// session. The real modular APMF lands on `main` separately.
//
// Foundation reused verbatim from prototype 1: the central Actor::Update(0xAD)
// vtable hook (every NPC routes through it), a crosshair-gated single test
// target with package-identity capture, the DirectInput hotkey sink, and tagged
// spdlog to Data/SKSE/Plugins/APMF.log. Prototype-1's own facets (KeepOffset
// walk / look-up / crouch) are KEPT as-is as movement/authority BASELINES to
// compare the new probes against.
//
// GAP TESTS ADDED (priority order, tag in [brackets]):
//   1. [minj] MOVEMENT-PROMOTE FEED (design §9.1, biggest unknown). The
//      IMovementDirectControl feed is un-named void(void) `Unk_01..08` in every
//      CommonLib fork -> calling it blind is the documented CTD roulette, so we
//      take the design's stated FALLBACK: an Actor::Move(0xC8) delta-INJECT of
//      OUR OWN non-zero toward-player vector (velocity we author, not a scale of
//      the engine's own delta like prototype 0). Reveals whether a real injected
//      velocity yields an ANIMATED walk that FACES travel (vs prototype-0's
//      slide, vs prototype-1's KeepOffset follow-glue) while the package stays
//      coherent. This decides how APMF drives movement.
//   2. [foe]/[pin] COMBAT-TARGET PIN (ch.6). StartCombat the gated actor against
//      the player (or a second aimed NPC) and log CombatController.targetHandle /
//      cachedTarget every ~1s -> does the threat system HOLD our target or
//      RE-SELECT? Resolves whether a target can be PINNED.
//   3. [cast] CASTING SELECT + TRIGGER (ch.8). Own the gated caster's
//      selectedSpells[right] with a known damage spell (Firebolt, guarded) and
//      log per tick whether the slot STAYS ours or the AI overwrites it, plus
//      the attack state -> is SELECTION a clean gate, and does the AI also cast
//      its OWN (the trigger-suppression gap)? Plus a CastSpellImmediate trigger.
//   4. [act] BEHAVIOR-TREE OBSERVE (ch.7, PASSIVE). Log the gated actor's
//      ATTACK_STATE_ENUM / block transitions (attack/block/power/bash) to expose
//      any observable lever. No driving.
//
// CONSTRAINTS honored: gated to one test target; VR-refused; version-robust
// vfunc-index + Address-Library + CommonLib accessors (no hand offsets); every
// struct read guarded + logged; no blind vtable call; no CTD path.
// ============================================================================

namespace {

    // ---- Address-Library-bound engine functions (prototype-1 baseline) ------
    namespace Native {
        void KeepOffsetFromActor(RE::Actor* a_actor, const RE::ActorHandle& a_target,
                                 const RE::NiPoint3& a_posOffset, const RE::NiPoint3& a_angleOffset,
                                 float a_catchUpRadius, float a_followRadius) {
            using func_t = void (*)(RE::Actor*, const RE::ActorHandle&, const RE::NiPoint3&,
                                    const RE::NiPoint3&, float, float);
            REL::Relocation<func_t> func{ RELOCATION_ID(36870, 37894) };
            func(a_actor, a_target, a_posOffset, a_angleOffset, a_catchUpRadius, a_followRadius);
        }
        void ClearKeepOffsetFromActor(RE::Actor* a_actor) {
            using func_t = void (*)(RE::Actor*);
            REL::Relocation<func_t> func{ RELOCATION_ID(36871, 37895) };
            func(a_actor);
        }
        void SetDontMove(RE::Actor* a_actor, bool a_dontMove) {
            using func_t = void (*)(RE::Actor*, bool);
            REL::Relocation<func_t> func{ RELOCATION_ID(36490, 37489) };
            func(a_actor, a_dontMove);
        }
        // Actor::StartCombat is NOT bound in this CommonLib rev -- use po3's
        // published Address-Library ID (MFO/Probe.cpp uses the same). SE/AE only;
        // no sourced VR id, so VR is refused upstream at InstallHooks.
        bool StartCombatOn(RE::Actor* a_actor, RE::Actor* a_target) {
            if (REL::Module::IsVR()) return false;
            using func_t = bool (*)(RE::Actor*, RE::Actor*, void*);
            static REL::Relocation<func_t> func{ REL::RelocationID(37608, 38561) };
            return func(a_actor, a_target, nullptr);
        }
    }

    // ---- Log ---------------------------------------------------------------
    void SetupLog() {
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;
        try {
            sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("Data/SKSE/Plugins/APMF.log", true);
        } catch (const spdlog::spdlog_ex&) {
            if (auto dir = SKSE::log::log_directory()) {
                try {
                    sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "APMF.log").string(), true);
                } catch (const spdlog::spdlog_ex&) { return; }
            } else { return; }
        }
        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);   // probe: flush every line so a CTD keeps the trail
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    }

    // ---- Controls (DirectInput scancodes) ----------------------------------
    // Prototype-1 BASELINES (kept):
    constexpr std::uint32_t kKeyMoveEngage    = 0x4F;   // Numpad1 : KeepOffset walk-to-player ENGAGE (manufacture baseline)
    constexpr std::uint32_t kKeyMoveDisengage = 0x51;   // Numpad3 : KeepOffset DISENGAGE
    constexpr std::uint32_t kKeyLookUpToggle  = 0x4B;   // Numpad4 : look-straight-up TOGGLE
    constexpr std::uint32_t kKeyCrouchToggle  = 0x47;   // Numpad7 : crouch TOGGLE
    // GAP TEST 1 — movement Move-inject:
    constexpr std::uint32_t kKeyMinjEngage    = 0x4C;   // Numpad5 : MOVE-INJECT engage (Move 0xC8 toward-player velocity)
    constexpr std::uint32_t kKeyMinjDisengage = 0x50;   // Numpad2 : MOVE-INJECT disengage
    // GAP TEST 2 — combat-target pin:
    constexpr std::uint32_t kKeyFoeCapture    = 0x4D;   // Numpad6 : capture crosshair actor as desired COMBAT FOE
    constexpr std::uint32_t kKeyCombatPin     = 0x49;   // Numpad9 : StartCombat gated-vs-foe(or player) + per-tick target log
    constexpr std::uint32_t kKeyCombatStop    = 0x4A;   // NumpadMinus : StopCombat + stop combat log
    // GAP TEST 3 — casting select + trigger:
    constexpr std::uint32_t kKeyCastOwn       = 0x48;   // Numpad8 : own selectedSpells[right]=Firebolt + per-tick cast observe
    constexpr std::uint32_t kKeyCastTrigger   = 0x37;   // Numpad* : CastSpellImmediate the owned spell at foe/player
    constexpr std::uint32_t kKeyCastRelease   = 0x4E;   // NumpadPlus : release spell ownership / stop cast observe
    // GAP TEST 4 — behavior-tree observe (passive):
    constexpr std::uint32_t kKeyActObsToggle  = 0x52;   // Numpad0 : toggle attack/block state-transition observe

    // KeepOffset tuning (baseline).
    constexpr float kCatchUpRadius = 100.0f;
    constexpr float kFollowRadius  = 50.0f;
    constexpr float kLookUpHeight  = 300.0f;
    constexpr std::uint64_t kMoveReassertEvery = 180;   // ~3s @ 60fps
    constexpr std::uint64_t kObsEvery          = 60;    // ~1s @ 60fps

    // Move-inject tuning: author a real walk-speed velocity toward the player.
    constexpr float kInjSpeed = 200.0f;   // world units / second (~walk)
    constexpr float kInjStopDist = 128.0f; // stop injecting inside this radius (don't shove into the player)

    // Known damage spell for the casting probe (guarded — a miss is non-fatal).
    constexpr RE::FormID kFirebolt = 0x0001C789;   // Skyrim.esm Firebolt (aimed, visible)
    constexpr RE::FormID kFlames   = 0x00012FCD;   // Skyrim.esm Flames (concentration) — fallback

    // ---- Gated state (main / sim thread only) ------------------------------
    // prototype-1 facets:
    std::atomic<bool>          g_moveOn{ false };
    std::atomic<bool>          g_lookUpOn{ false };
    std::atomic<bool>          g_crouchOn{ false };
    // gap-test channels:
    std::atomic<bool>          g_minjOn{ false };       // movement inject
    std::atomic<bool>          g_combatLogOn{ false };  // per-tick combat-target log
    std::atomic<bool>          g_castOwnOn{ false };    // owning selectedSpells + cast observe
    std::atomic<bool>          g_actObsOn{ false };     // attack-state transition observe

    std::atomic<RE::Actor*>    g_targetPtr{ nullptr };  // the gated test actor (fast identity compare in the thunk)
    RE::ActorHandle            g_targetHandle{};        // liveness source of truth
    RE::ActorHandle            g_playerHandle{};        // KeepOffset / move anchor
    RE::ActorHandle            g_foeHandle{};           // desired combat foe (ch.6) / cast trigger victim
    RE::FormID                 g_pkgAtCapture{ 0 };     // package the target held when captured
    RE::FormID                 g_ownedSpell{ 0 };       // spell we forced into selectedSpells[right]
    std::uint32_t              g_lastAttackState{ 0xFF };
    int                        g_lastBlocking{ -1 };
    std::atomic<std::uint64_t> g_ticks{ 0 };

    // Any probe channel that needs the per-tick hook body to run for the gated actor.
    bool AnyFacetOn() {
        return g_moveOn.load(std::memory_order_relaxed) ||
               g_lookUpOn.load(std::memory_order_relaxed) ||
               g_crouchOn.load(std::memory_order_relaxed);
    }
    bool AnyProbeActive() {
        return AnyFacetOn() ||
               g_minjOn.load(std::memory_order_relaxed) ||
               g_combatLogOn.load(std::memory_order_relaxed) ||
               g_castOwnOn.load(std::memory_order_relaxed) ||
               g_actObsOn.load(std::memory_order_relaxed);
    }

    const char* PkgTypeName(RE::TESPackage* a_pkg) {
        if (!a_pkg) return "<none>";
        const char* n = a_pkg->GetObjectTypeName();
        return n ? n : "<unnamed>";
    }
    const char* AttackStateName(RE::ATTACK_STATE_ENUM s) {
        switch (s) {
        case RE::ATTACK_STATE_ENUM::kNone:          return "None";
        case RE::ATTACK_STATE_ENUM::kDraw:          return "Draw";
        case RE::ATTACK_STATE_ENUM::kSwing:         return "Swing";
        case RE::ATTACK_STATE_ENUM::kHit:           return "Hit";
        case RE::ATTACK_STATE_ENUM::kNextAttack:    return "NextAttack";
        case RE::ATTACK_STATE_ENUM::kFollowThrough: return "FollowThrough";
        case RE::ATTACK_STATE_ENUM::kBash:          return "Bash";
        default:                                    return "?";
        }
    }

    // Resolve the gated target: prefer the crosshair actor (aim to pick); fall
    // back to the already-captured one. (Re)captures package identity.
    RE::Actor* EnsureTarget() {
        RE::Actor* fromCrosshair = nullptr;
        if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
            if (auto ref = pick->targetActor.get()) fromCrosshair = ref->As<RE::Actor>();
        }
        if (fromCrosshair && !fromCrosshair->IsPlayerRef()) {
            if (fromCrosshair != g_targetPtr.load()) {
                auto* pkg = fromCrosshair->GetCurrentPackage();
                g_pkgAtCapture = pkg ? pkg->GetFormID() : 0;
                g_targetHandle = fromCrosshair->GetHandle();
                g_targetPtr.store(fromCrosshair);
                spdlog::info("[target] captured 0x{:08X} '{}' pkg=0x{:08X}({})",
                             fromCrosshair->GetFormID(),
                             fromCrosshair->GetName() ? fromCrosshair->GetName() : "?",
                             g_pkgAtCapture, PkgTypeName(pkg));
            }
            return fromCrosshair;
        }
        if (auto* kept = g_targetPtr.load(); kept && g_targetHandle.get()) return kept;
        spdlog::warn("[target] REFUSED -- no actor under the crosshair. Aim directly at a follower/NPC.");
        return nullptr;
    }

    void ClearTargetIfIdle() {
        if (AnyProbeActive()) return;
        g_targetPtr.store(nullptr);
        g_targetHandle = RE::ActorHandle{};
        g_pkgAtCapture = 0;
    }

    // ---- BASELINE facet: KeepOffset MOVEMENT (walk to player, manufacture) ---
    void MoveEngage() {
        auto* target = EnsureTarget();
        if (!target) return;
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) return;
        g_playerHandle = pc->GetHandle();
        Native::SetDontMove(target, false);
        Native::KeepOffsetFromActor(target, g_playerHandle, RE::NiPoint3{ 0.0f, 0.0f, 0.0f },
                                    RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, kCatchUpRadius, kFollowRadius);
        g_moveOn.store(true);
        spdlog::info("[move] BASELINE ENGAGE tgt=0x{:08X} KeepOffsetFromActor(player). dist={:.1f}",
                     target->GetFormID(), target->GetPosition().GetDistance(pc->GetPosition()));
    }
    void MoveDisengage() {
        auto* target = g_targetPtr.load();
        if (g_moveOn.exchange(false) && target) {
            Native::ClearKeepOffsetFromActor(target);
            auto* pkg = target->GetCurrentPackage();
            spdlog::info("[move] BASELINE DISENGAGE tgt=0x{:08X} pkg-now=0x{:08X}({}) pkg-at-capture=0x{:08X}",
                         target->GetFormID(), pkg ? pkg->GetFormID() : 0, PkgTypeName(pkg), g_pkgAtCapture);
        }
        ClearTargetIfIdle();
    }

    // ---- BASELINE facet: LOOK STRAIGHT UP -----------------------------------
    void LookUpToggle() {
        if (g_lookUpOn.load()) {
            g_lookUpOn.store(false);
            spdlog::info("[look] DISENGAGE -- stop asserting headtrack.");
            ClearTargetIfIdle();
            return;
        }
        if (!EnsureTarget()) return;
        g_lookUpOn.store(true);
        spdlog::info("[look] ENGAGE -- craning head up; re-asserted every tick.");
    }
    bool AssertLookUp(RE::Actor* a_actor) {
        auto* proc = a_actor->GetActorRuntimeData().currentProcess;
        if (!proc) return false;
        const bool aiRepointed = static_cast<bool>(proc->GetHeadtrackTarget());
        RE::NiPoint3 up = a_actor->GetPosition();
        up.z += kLookUpHeight;
        proc->SetHeadtrackTarget(a_actor, up);
        return aiRepointed;
    }

    // ---- BASELINE facet: CROUCH ---------------------------------------------
    void CrouchToggle() {
        if (g_crouchOn.load()) {
            g_crouchOn.store(false);
            if (auto* t = g_targetPtr.load()) t->NotifyAnimationGraph("SneakStop"sv);
            spdlog::info("[crouch] DISENGAGE -- SneakStop.");
            ClearTargetIfIdle();
            return;
        }
        auto* target = EnsureTarget();
        if (!target) return;
        target->NotifyAnimationGraph("SneakStart"sv);
        g_crouchOn.store(true);
        spdlog::info("[crouch] ENGAGE -- SneakStart; re-asserted if the AI stands her up.");
    }
    bool AssertCrouch(RE::Actor* a_actor) {
        if (a_actor->IsSneaking()) return false;
        a_actor->NotifyAnimationGraph("SneakStart"sv);
        return true;
    }

    // ============================================================================
    // GAP TEST 1 — [minj] MOVEMENT-PROMOTE via Actor::Move(0xC8) delta-inject.
    // Design §9.1 fallback. We AUTHOR a non-zero toward-player velocity vector and
    // feed it to the last-mile locomotion vfunc every tick, then observe whether
    // real locomotion animation + facing result, while the package stays coherent.
    // ============================================================================
    void MinjEngage() {
        auto* target = EnsureTarget();
        if (!target) return;
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) return;
        g_playerHandle = pc->GetHandle();
        Native::SetDontMove(target, false);
        g_minjOn.store(true);
        spdlog::info("[minj] ENGAGE tgt=0x{:08X} -> per-tick Actor::Move(0xC8) inject of an AUTHORED "
                     "toward-player velocity (~{:.0f} u/s). Watch: does she WALK with animation and FACE "
                     "the travel direction, or slide? dist={:.1f}",
                     target->GetFormID(), kInjSpeed, target->GetPosition().GetDistance(pc->GetPosition()));
    }
    void MinjDisengage() {
        if (g_minjOn.exchange(false)) {
            auto* t = g_targetPtr.load();
            auto* pkg = t ? t->GetCurrentPackage() : nullptr;
            spdlog::info("[minj] DISENGAGE tgt=0x{:08X} pkg-now=0x{:08X}({}) pkg-at-capture=0x{:08X} "
                         "-> package's own locomotion resumes.",
                         t ? t->GetFormID() : 0, pkg ? pkg->GetFormID() : 0, PkgTypeName(pkg), g_pkgAtCapture);
        }
        ClearTargetIfIdle();
    }
    // Author + inject one frame of velocity. Returns injected magnitude (0 = held).
    void MinjTick(RE::Actor* a_actor, float a_delta) {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) return;
        RE::NiPoint3 toward = pc->GetPosition() - a_actor->GetPosition();
        toward.z = 0.0f;
        const float dist = toward.Length();   // z zeroed -> horizontal distance
        if (dist <= kInjStopDist || dist < 1.0f) return;   // arrived / degenerate: inject nothing
        const float inv = 1.0f / dist;
        RE::NiPoint3 injected{ toward.x * inv * kInjSpeed * a_delta,
                               toward.y * inv * kInjSpeed * a_delta, 0.0f };
        // Actor::Move(0xC8): last-mile locomotion delta. Called directly through the
        // vtable (no blind unnamed feed). This is the design's documented fallback.
        a_actor->Move(a_delta, injected);
    }

    // ============================================================================
    // GAP TEST 2 — [foe]/[pin] COMBAT-TARGET PIN (ch.6).
    // ============================================================================
    void FoeCapture() {
        RE::Actor* foe = nullptr;
        if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
            if (auto ref = pick->targetActor.get()) foe = ref->As<RE::Actor>();
        }
        if (!foe) { spdlog::warn("[foe] REFUSED -- aim at the intended enemy NPC first."); return; }
        g_foeHandle = foe->GetHandle();
        spdlog::info("[foe] captured desired COMBAT FOE 0x{:08X} '{}'. Now aim at the FIGHTER and press "
                     "Numpad9 to StartCombat it against this foe.",
                     foe->GetFormID(), foe->GetName() ? foe->GetName() : "?");
    }
    void CombatPin() {
        auto* fighter = EnsureTarget();
        if (!fighter) return;
        RE::Actor* foe = g_foeHandle.get().get();
        const char* foeSrc = "captured-foe";
        if (!foe) { foe = RE::PlayerCharacter::GetSingleton(); foeSrc = "player(default)"; }
        if (!foe) return;
        const bool ok = Native::StartCombatOn(fighter, foe);
        g_combatLogOn.store(true);
        spdlog::info("[pin] StartCombat fighter=0x{:08X} -> foe=0x{:08X} ({}) returned {}. Per-tick "
                     "CombatController.targetHandle/cachedTarget log ON; HELD vs RESELECTED tells us if a "
                     "target can be pinned.",
                     fighter->GetFormID(), foe->GetFormID(), foeSrc, ok ? "true" : "false");
    }
    void CombatStop() {
        auto* fighter = g_targetPtr.load();
        if (g_combatLogOn.exchange(false) && fighter) {
            fighter->StopCombat();
            spdlog::info("[pin] StopCombat + combat log OFF (fighter=0x{:08X}).", fighter->GetFormID());
        }
        ClearTargetIfIdle();
    }
    void CombatTick(RE::Actor* a_actor) {
        auto* cc = a_actor->GetActorRuntimeData().combatController;
        if (!cc) { spdlog::info("[pin] combatController=NULL (not in a combat group)."); return; }
        RE::Actor* curTgt = cc->targetHandle.get().get();
        RE::Actor* cached = cc->cachedTarget.get();
        RE::Actor* desired = g_foeHandle.get().get();
        if (!desired) desired = RE::PlayerCharacter::GetSingleton();
        const RE::FormID curId = curTgt ? curTgt->GetFormID() : 0;
        const RE::FormID desId = desired ? desired->GetFormID() : 0;
        const bool held = (curId != 0 && curId == desId);
        spdlog::info("[pin] combat: targetHandle=0x{:08X} cachedTarget=0x{:08X} desired=0x{:08X} [{}] "
                     "prevTarget=0x{:08X} fleeing={} startedCombat={}",
                     curId, cached ? cached->GetFormID() : 0, desId, held ? "HELD" : "RESELECTED/OTHER",
                     cc->previousTargetHandle.get() ? cc->previousTargetHandle.get().get()->GetFormID() : 0,
                     cc->state ? (cc->state->isFleeing ? 1 : 0) : -1, cc->startedCombat ? 1 : 0);
    }

    // ============================================================================
    // GAP TEST 3 — [cast] CASTING SELECT + TRIGGER (ch.8).
    // ============================================================================
    RE::SpellItem* ResolveProbeSpell(RE::Actor* a_actor) {
        if (auto* s = RE::TESForm::LookupByID<RE::SpellItem>(kFirebolt)) return s;
        spdlog::warn("[cast] Firebolt 0x{:08X} not found -> trying Flames 0x{:08X}.", kFirebolt, kFlames);
        if (auto* s = RE::TESForm::LookupByID<RE::SpellItem>(kFlames)) return s;
        // last resort: whatever the actor already has in the right hand
        auto* existing = a_actor->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];
        spdlog::warn("[cast] Flames missing too -> falling back to actor's existing right-hand spell 0x{:08X}.",
                     existing ? existing->GetFormID() : 0);
        return existing ? existing->As<RE::SpellItem>() : nullptr;
    }
    void CastOwn() {
        auto* target = EnsureTarget();
        if (!target) return;
        auto& rt = target->GetActorRuntimeData();
        // log the BEFORE picture (both hands) so the tester sees pre-existing selection
        auto* preL = rt.selectedSpells[RE::Actor::SlotTypes::kLeftHand];
        auto* preR = rt.selectedSpells[RE::Actor::SlotTypes::kRightHand];
        spdlog::info("[cast] BEFORE own: leftSel=0x{:08X} rightSel=0x{:08X}",
                     preL ? preL->GetFormID() : 0, preR ? preR->GetFormID() : 0);
        RE::SpellItem* spell = ResolveProbeSpell(target);
        if (!spell) { spdlog::warn("[cast] no usable probe spell resolved -- aborting own."); return; }
        // OWN the right-hand selection. SetCurrentSpell is NOT bound at this
        // CommonLib rev (only the no-op SetCurrentSpellImpl), so we write the slot
        // member AND the caster's currentSpell member directly. This probe wants
        // exactly that write to see if the AI honors the selection (a clean gate)
        // or overwrites it (the trigger-suppression gap). Bookkeeping-desync
        // caveat noted -- fine for a throwaway observation.
        rt.selectedSpells[RE::Actor::SlotTypes::kRightHand] = spell;
        if (auto* caster = target->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand)) {
            caster->currentSpell = spell;
        }
        g_ownedSpell = spell->GetFormID();
        g_castOwnOn.store(true);
        spdlog::info("[cast] OWN tgt=0x{:08X} rightSel := 0x{:08X} '{}'. Set ONCE, NOT re-asserted. Per-tick "
                     "[cast] log reports KEPT vs AI-OVERWROTE (is selection a clean gate?) and the attack "
                     "state. Watch the game: does she cast OUR spell, HER own, both, or nothing?",
                     target->GetFormID(), g_ownedSpell, spell->GetName() ? spell->GetName() : "?");
    }
    void CastTrigger() {
        auto* target = g_targetPtr.load();
        if (!target) { spdlog::warn("[cast] trigger REFUSED -- no gated target."); return; }
        auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(g_ownedSpell ? g_ownedSpell : kFirebolt);
        if (!spell) { spdlog::warn("[cast] trigger REFUSED -- probe spell unresolved."); return; }
        RE::Actor* victim = g_foeHandle.get().get();
        if (!victim) victim = RE::PlayerCharacter::GetSingleton();
        auto* caster = target->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand);
        if (!caster) { spdlog::warn("[cast] trigger REFUSED -- no right-hand magic caster."); return; }
        caster->CastSpellImmediate(spell, false, victim, 1.0f, false, 0.0f, target);
        spdlog::info("[cast] TRIGGER CastSpellImmediate(0x{:08X}) tgt=0x{:08X} -> victim=0x{:08X}. "
                     "Additive injection (MFO's path) -- fires regardless of the AI trigger.",
                     spell->GetFormID(), target->GetFormID(), victim ? victim->GetFormID() : 0);
    }
    void CastRelease() {
        if (g_castOwnOn.exchange(false)) {
            g_ownedSpell = 0;
            spdlog::info("[cast] RELEASE -- stop observing/owning the spell slot.");
        }
        ClearTargetIfIdle();
    }
    void CastTick(RE::Actor* a_actor) {
        auto& rt = a_actor->GetActorRuntimeData();
        auto* rightSel = rt.selectedSpells[RE::Actor::SlotTypes::kRightHand];
        const RE::FormID rightId = rightSel ? rightSel->GetFormID() : 0;
        RE::MagicItem* cur = nullptr;
        if (auto* caster = a_actor->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand)) {
            cur = caster->currentSpell;
        }
        const bool kept = (rightId != 0 && rightId == g_ownedSpell);
        auto* as = a_actor->AsActorState();
        spdlog::info("[cast] observe: rightSel=0x{:08X}(ours=0x{:08X}) [{}] currentSpell=0x{:08X} attackState={} "
                     "weaponDrawn={}",
                     rightId, g_ownedSpell, kept ? "KEPT" : "AI-OVERWROTE",
                     cur ? cur->GetFormID() : 0, as ? AttackStateName(as->GetAttackState()) : "?",
                     (as && as->IsWeaponDrawn()) ? 1 : 0);
    }

    // ============================================================================
    // GAP TEST 4 — [act] BEHAVIOR-TREE OBSERVE (ch.7, passive; transitions only).
    // ============================================================================
    void ActObsToggle() {
        if (g_actObsOn.load()) {
            g_actObsOn.store(false);
            spdlog::info("[act] observe OFF.");
            ClearTargetIfIdle();
            return;
        }
        if (!EnsureTarget()) return;
        g_lastAttackState = 0xFF; g_lastBlocking = -1;
        g_actObsOn.store(true);
        spdlog::info("[act] observe ON -- logging attack/block/power/bash state TRANSITIONS (passive).");
    }
    void ActObsTick(RE::Actor* a_actor) {
        auto* as = a_actor->AsActorState();
        if (!as) return;
        const std::uint32_t st = static_cast<std::uint32_t>(as->GetAttackState());
        const int blk = a_actor->IsBlocking() ? 1 : 0;
        if (st == g_lastAttackState && blk == g_lastBlocking) return;   // transitions only, no spam
        spdlog::info("[act] transition: attackState {} -> {} | blocking {} -> {} | weaponDrawn={}",
                     g_lastAttackState == 0xFF ? "?" : AttackStateName(static_cast<RE::ATTACK_STATE_ENUM>(g_lastAttackState)),
                     AttackStateName(as->GetAttackState()),
                     g_lastBlocking < 0 ? -1 : g_lastBlocking, blk, as->IsWeaponDrawn() ? 1 : 0);
        g_lastAttackState = st;
        g_lastBlocking = blk;
    }

    void ReleaseAllFacets(const char* a_why) {
        if (!AnyProbeActive()) return;
        auto* t = g_targetPtr.load();
        if (g_moveOn.exchange(false) && t) Native::ClearKeepOffsetFromActor(t);
        if (g_crouchOn.exchange(false) && t) t->NotifyAnimationGraph("SneakStop"sv);
        if (g_combatLogOn.exchange(false) && t) t->StopCombat();
        g_lookUpOn.store(false);
        g_minjOn.store(false);
        g_castOwnOn.store(false);
        g_actObsOn.store(false);
        g_ownedSpell = 0;
        spdlog::info("[release] all probe channels released ({})", a_why);
        ClearTargetIfIdle();
    }

    // ---- Input sink --------------------------------------------------------
    class InputSink : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static InputSink* GetSingleton() { static InputSink s; return &s; }
        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
                                              RE::BSTEventSource<RE::InputEvent*>*) override {
            if (!a_event) return RE::BSEventNotifyControl::kContinue;
            for (auto* e = *a_event; e; e = e->next) {
                if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
                auto* btn = e->AsButtonEvent();
                if (!btn || !btn->IsDown()) continue;
                if (btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;
                switch (btn->GetIDCode()) {
                case kKeyMoveEngage:    MoveEngage();    break;
                case kKeyMoveDisengage: MoveDisengage(); break;
                case kKeyLookUpToggle:  LookUpToggle();  break;
                case kKeyCrouchToggle:  CrouchToggle();  break;
                case kKeyMinjEngage:    MinjEngage();    break;
                case kKeyMinjDisengage: MinjDisengage(); break;
                case kKeyFoeCapture:    FoeCapture();    break;
                case kKeyCombatPin:     CombatPin();     break;
                case kKeyCombatStop:    CombatStop();    break;
                case kKeyCastOwn:       CastOwn();       break;
                case kKeyCastTrigger:   CastTrigger();   break;
                case kKeyCastRelease:   CastRelease();   break;
                case kKeyActObsToggle:  ActObsToggle();  break;
                default: break;
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ---- The central hook: Actor::Update @ 0xAD ----------------------------
    struct CharacterUpdateHook {
        static void thunk(RE::Actor* a_this, float a_delta) {
            func(a_this, a_delta);   // original FIRST -- probes layer on top of the real AI tick
            if (!AnyProbeActive()) return;
            if (a_this != g_targetPtr.load(std::memory_order_relaxed)) return;
            if (!g_targetHandle.get()) { ReleaseAllFacets("target-unloaded"); return; }

            const std::uint64_t tick = g_ticks.fetch_add(1, std::memory_order_relaxed);

            // --- prototype-1 baseline authority (re-assert) ---
            bool headFought = false, crouchFought = false;
            if (g_lookUpOn.load(std::memory_order_relaxed)) headFought   = AssertLookUp(a_this);
            if (g_crouchOn.load(std::memory_order_relaxed)) crouchFought = AssertCrouch(a_this);
            if (g_moveOn.load(std::memory_order_relaxed) && (tick % kMoveReassertEvery) == 0) {
                if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                    g_playerHandle = pc->GetHandle();
                    Native::KeepOffsetFromActor(a_this, g_playerHandle, RE::NiPoint3{ 0.0f, 0.0f, 0.0f },
                                                RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, kCatchUpRadius, kFollowRadius);
                }
            }

            // --- GAP 1: movement inject (every tick) ---
            if (g_minjOn.load(std::memory_order_relaxed)) MinjTick(a_this, a_delta);

            // --- GAP 4: behavior-tree observe (every tick, transitions only) ---
            if (g_actObsOn.load(std::memory_order_relaxed)) ActObsTick(a_this);

            if (headFought)
                spdlog::info("[look] AI repointed the head -> re-asserted straight-up");
            if (crouchFought)
                spdlog::info("[crouch] AI stood her up -> re-asserted SneakStart");

            // --- rate-limited (~1/s) observability ---
            if ((tick % kObsEvery) != 0) return;

            // GAP 2: combat-target
            if (g_combatLogOn.load(std::memory_order_relaxed)) CombatTick(a_this);
            // GAP 3: casting select observe
            if (g_castOwnOn.load(std::memory_order_relaxed))  CastTick(a_this);

            // package-coherence heartbeat (every probe)
            auto*      pkg  = a_this->GetCurrentPackage();
            RE::FormID id   = pkg ? pkg->GetFormID() : 0;
            const bool same = (id == g_pkgAtCapture);
            float dist = 0.0f;
            if (auto* pc = RE::PlayerCharacter::GetSingleton()) dist = a_this->GetPosition().GetDistance(pc->GetPosition());
            auto* as = a_this->AsActorState();
            spdlog::info("[obs] tgt=0x{:08X} pkg=0x{:08X}({}) [{}] chan[minj={} pin={} cast={} act={} kMove={} look={} crouch={}] "
                         "moving[walk={} sprint={}] dist-to-player={:.1f}",
                         a_this->GetFormID(), id, PkgTypeName(pkg),
                         same ? "PACKAGE STABLE" : "PACKAGE CHANGED!!",
                         g_minjOn.load() ? 1 : 0, g_combatLogOn.load() ? 1 : 0, g_castOwnOn.load() ? 1 : 0,
                         g_actObsOn.load() ? 1 : 0, g_moveOn.load() ? 1 : 0, g_lookUpOn.load() ? 1 : 0,
                         g_crouchOn.load() ? 1 : 0,
                         (as && as->IsWalking()) ? 1 : 0, (as && as->IsSprinting()) ? 1 : 0, dist);
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr std::size_t idx = 0x0AD;   // Actor::Update(float)
    };

    struct PlayerUpdateHook {
        static void thunk(RE::PlayerCharacter* a_this, float a_delta) {
            func(a_this, a_delta);
            static bool s_first = true;
            if (s_first) { s_first = false; spdlog::info("[hook] player Update seat live (0xAD firing)"); }
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr std::size_t idx = 0x0AD;
    };

    std::atomic<bool> g_hooksInstalled{ false };

    void InstallHooks() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[hook] VR runtime -- 0xAD index unverified for VR; hooks NOT installed.");
            return;
        }
        if (g_hooksInstalled.exchange(true)) return;

        REL::Relocation<std::uintptr_t> charVtbl{ RE::VTABLE_Character[0] };
        CharacterUpdateHook::func = charVtbl.write_vfunc(CharacterUpdateHook::idx, CharacterUpdateHook::thunk);

        REL::Relocation<std::uintptr_t> pcVtbl{ RE::VTABLE_PlayerCharacter[0] };
        PlayerUpdateHook::func = pcVtbl.write_vfunc(PlayerUpdateHook::idx, PlayerUpdateHook::thunk);

        spdlog::info("[hook] installed: Character Update(0x{:X}) + PlayerCharacter Update(0x{:X}). Central "
                     "0xAD seat live for every NPC.", CharacterUpdateHook::idx, PlayerUpdateHook::idx);
    }

    void LogBanner() {
        spdlog::info("=== APMF PROBE/GAPS hotkeys (aim crosshair at a follower/NPC first) ===");
        spdlog::info("  BASELINE: Numpad1=KeepOffset walk-to-player  Numpad3=stop  Numpad4=look-up  Numpad7=crouch");
        spdlog::info("  GAP1 [minj] MOVEMENT inject:  Numpad5=engage (Move 0xC8 authored velocity)  Numpad2=disengage");
        spdlog::info("  GAP2 [foe]/[pin] COMBAT pin:  Numpad6=capture FOE   Numpad9=StartCombat fighter-vs-foe(+log)   NumpadMinus=StopCombat");
        spdlog::info("  GAP3 [cast] CASTING:          Numpad8=own right-hand spell(+observe)   Numpad*=CastSpellImmediate trigger   NumpadPlus=release");
        spdlog::info("  GAP4 [act] BEHAVIOR observe:  Numpad0=toggle attack/block transition log (passive)");
        spdlog::info("  Tags: [minj][foe][pin][cast][act][obs] -- grep the tag to read each finding.");
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            InstallHooks();
            if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
                idm->AddEventSink(InputSink::GetSingleton());
                LogBanner();
            }
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            ReleaseAllFacets("kPreLoadGame");
            break;
        default: break;
        }
    }

}   // namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    SetupLog();

    const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    const auto  ver    = plugin->GetVersion();
    spdlog::info("=== APMF {}.{}.{} (PROBE/GAPS) loading -- game {} ===",
                 ver.major(), ver.minor(), ver.patch(),
                 REL::Module::get().version().string());

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);

    spdlog::info("=== APMF loaded (PROBE/GAPS: movement-inject + combat-pin + casting + behavior-observe) ===");
    return true;
}
