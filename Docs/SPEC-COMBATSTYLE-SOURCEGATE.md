# SPEC — Combat-Style Re-Assert: movement interference, proof toggle, and the source-gate fix

> **STATUS BANNER (added 2026-09-07) — §1-6 SHIPPED (in MFO), §7 SUPERSEDED, AND THIS FILE
> IS IN THE WRONG REPOSITORY.**
> - **It is an MFO spec.** Every `file:line` in it is MFO's `native/`, the fix shipped in
>   MFO (`Config.h` `g_cstyReassert`, `CombatStyle.cpp`, the live flip at `plugin.cpp`), and
>   nothing in it changes APMF code. It lives here only because of where the pass was run.
>   **Flagged, deliberately NOT moved:** moving it means writing to the MFO repo, which this
>   change was not authorised to do. Whoever picks that up should move it to MFO's `Docs/`
>   and leave a pointer here.
> - **§7 is SUPERSEDED.** It defers the heal-animation gap ("both A and B"); heals now animate
>   through the ch.8b engine seats, shipped in APMF v0.9.1.
> - **`:230` is out of date:** "could not grep the pinned CommonLib locally" — the pinned tree
>   IS available locally at `/mnt/gaming/modlists/Projects/_commonlib/pinned-3.7.0-c4ab853d/`
>   (CLAUDE.md), and it is what CI compiles against. Re-check any symbol claim in this file
>   against it rather than trusting the file's own caveat.
> - It was never committed until 2026-09-07: it lived as an untracked working-tree file.

Status: **research + fix-spec** (read-only investigation, 2026-09-04). No code changed.
Scope: MFO (`marth-follower-overhaul`) facet #5 combat-style bias + APMF channel gap.
Field symptom (marth, MFO 2.0.0 proving session): **ALL followers, COMBAT-ONLY,
INTERMITTENT movement stutter — they do not commit to a position.** Prime suspect:
the CSTY re-assert loop in `native/CombatStyle.cpp`, the last per-tick re-assert left
in the codebase.

All `file:line` below are MFO's `native/` unless prefixed `APMF:`.

---

## 0. Six-line summary

1. **Mechanism confirmed:** `CombatStyle::ApplyTick` (CombatStyle.cpp:138), driven every combat tick per combatant from the `UpdateCombat` hook (Targeting.cpp:62), re-writes `cc->combatStyle` whenever the engine re-derives it away from MFO's stance style (CombatStyle.cpp:189-201). This is the only per-tick re-assert left.
2. **Causation is PLAUSIBLE but not statically provable:** the write itself calls no re-eval; the stutter comes from MFO's style and the engine's re-derived style specifying *different preferred combat distances*, so the movement planner's goal flips between ticks. Honest verdict: needs the A/B toggle to confirm.
3. **A/B proof toggle:** add `bCstyReassert` (default ON), gate ONLY the re-derive re-assert branch (CombatStyle.cpp:189-201); leave the initial-ownership and handoff writes intact. Flip OFF mid-battle via a dev hotkey (mirror Board.cpp:1655-1671). OFF fixes positioning == proven.
4. **This batch AMPLIFIED it (regression: YES, likely):** facet #2 force-equips the weapon every hold tick (Actuation.cpp:894-895) and facet #1 flips the combat target (Actuation.cpp:1012, Targeting.cpp:118-119) — weapon-swap and target-change are exactly the events that trigger the engine's style re-derivation the loop fights.
5. **Cheapest interim fix is ALREADY in the code and does NOT help:** ApplyTick already writes only on change (the `else if (a_cc->combatStyle != target)` guard, CombatStyle.cpp:189). The engine reverts every tick, so "write-on-change" still writes every tick. No cheap mitigation resolves it; the real levers are stop-re-asserting or gate-the-source.
6. **Recommendation for 2.0.0:** ship the toggle, PROVE in one session. If proven, the fix is to STOP re-asserting and lean on the already-installed equip gate (facet #2, CombatStyle.cpp:252 EquipGateThunk) to hold the weapon — no new APMF channel needed for release. The true source-gate (own the style selection) has NO clean chainable vfunc today and stays post-release.

---

## 1. The re-assert mechanism (file:line)

### 1.1 Where it runs
`ApplyTick(actor, cc)` (CombatStyle.cpp:138) is called from the `UpdateCombat` vfunc
hook (`Character::UpdateCombat`, vtable 0xE4) at **Targeting.cpp:62**, after the
original runs (Targeting.cpp:44). That hook fires **every combat tick for every
combatant in the world** (Targeting.cpp:29-31 comment; CombatStyle.h:23-26). It is the
one callback that hands a live `CombatController*` to MFO each tick, which is why the
combat-style ownership lives here rather than on the job-worker scheduler.

The lock-free gate `CombatStyle::AnyActive()` (CombatStyle.cpp:75) keeps the common
no-stance tick off the mutex (Targeting.cpp:51,62).

### 1.2 The three write branches inside ApplyTick
After taking `g_mx` and finding the follower's owned entry (CombatStyle.cpp:151-154):

- **A. New controller** (`o.cc != a_cc`, CombatStyle.cpp:165-176): the previous fight's
  controller died and the engine built a fresh one from the base record. MFO captures
  the engine-derived style as the restore point (`o.saved`) and writes its stance style
  (`a_cc->combatStyle = target`, line 170). **One-shot per fight.** Legitimate.
- **B. Stance handoff** (`o.applied != o.stance`, CombatStyle.cpp:184-188): a
  melee↔ranged gambit flip. Writes once at the transition. Legitimate.
- **C. Engine RE-DERIVE re-assert** (`a_cc->combatStyle != target`, CombatStyle.cpp:189-201):
  **THE SUSPECT.** The engine reverted `cc->combatStyle` to the base-derived style
  mid-fight; MFO re-writes its stance style (`a_cc->combatStyle = target`, line 196),
  increments `o.rederives`, and logs only the first occurrence (lines 197-200 — this is
  the `[wstyle] ... engine RE-DERIVED the style under the swap -- re-asserting ... each
  tick` line marth saw).

### 1.3 What triggers "the engine RE-DERIVED the style"
MFO does not call the re-derivation; the engine does, internally, and MFO only *detects*
it after the fact (branch C's condition). The header states the trigger class directly:
the CSTY swap is a **score bias, not a prohibition** (CombatStyle.h:97-102) — a
spell-heavy caster's magic score can still beat the starved weapon score, "and the
engine RE-DERIVES the style mid-combat ... windows in which her own combat AI re-arms a
spell, strips the forced weapon, and MFO's equip rule fires again next tick." So the
re-derivation is **event-driven off weapon/loadout/target churn**, not a fixed
per-frame recompute. This is the key link to the regression question (§3).

### 1.4 How often MFO writes `cc->combatStyle`
Only when branch A/B/C fires. Branches A and B are one-shot. Branch C fires **on every
tick where the engine has reverted since the last tick** — i.e. throughout a
"re-derive episode." During a quiet episode (engine leaves the style alone) branch C
never fires and there is zero per-tick write. During an active episode the engine
reverts each tick, so `a_cc->combatStyle != target` is true each tick and MFO writes
each tick. The write is a single aligned pointer store, atomic on x64
(CombatStyle.cpp:16-18).

### 1.5 Does writing the same style value re-trigger positioning re-evaluation?
**No — not directly.** The write is a raw pointer store (CombatStyle.cpp:196); it calls
no engine function, dispatches no event, sets no dirty flag. **The positioning churn is
INDIRECT:** the combat behavior/movement planner reads `cc->combatStyle` each planning
cycle for approach/retreat distance, `avoidThreat`, circle/flank/reposition weights
(CombatStyle.h:16-18 — "MFO_MeleeStyle, bow starved, avoidThreat 0, so the AI closes
and swings"). During a re-derive episode the field alternates between two styles with
**different preferred combat distances**:

- engine's re-derived base style (for an archer: ranged-favoring, large standoff) → planner wants to back off;
- MFO's stance style (melee: avoidThreat 0, close-and-swing) → planner wants to close.

The planner sees a different goal distance on alternating ticks and never settles → the
follower starts toward one goal, the goal flips, it re-plans → **stutter, no
commitment.** This matches the field report precisely: COMBAT-ONLY (combatStyle is read
only in combat), INTERMITTENT (only during an active re-derive episode), ALL followers
(bWeaponStyleControl is default ON, Config.cpp:286; every follower with an owned weapon
stance is eligible).

**Ordering caveat (flagged uncertainty).** The original `UpdateCombat` runs *before*
ApplyTick (Targeting.cpp:44 then :62). So within one tick, the engine's planning inside
the original reads whatever `combatStyle` currently is, and MFO's write lands after.
Whether the visible flip is "engine value during planning, MFO value after" or a true
per-tick alternation depends on exactly when in the frame the engine re-derives vs.
plans — which static analysis cannot pin down. Either way the observable is a style/goal
that does not hold still across ticks. **This is the honest reason the A/B toggle, not
more reading, is the proof.** (§2)

---

## 2. A/B proof toggle — `bCstyReassert` (the empirical proof)

**Goal:** a mid-battle-flippable switch that disables ONLY the re-derive re-assert loop
(branch C), so marth can watch positioning with the loop ON vs OFF in one fight.
Re-assert OFF → stutter stops == the re-assert is the cause (proven).

### 2.1 Config declaration (mirror the existing dev-toggle pattern)
- **Config.h** (near the weapon-style kill-switch, ~line 367):
  `inline std::atomic<bool> g_cstyReassert{ true };` — default ON = today's behavior.
- **Config.cpp** parse (near line 129, beside `bWeaponStyleControl`):
  `else if (a_key == "bCstyReassert") setB(g_cstyReassert);`
- **Config.cpp** reset block (near line 286):
  `g_cstyReassert = true;`
This is INI-only, no MCM — the `bProbeCastStyle`/`bWeaponStyleControl` precedent
(Config.h:244, Config.cpp:122,129).

### 2.2 The gate — EXACTLY the re-derive branch, nothing else
In `ApplyTick`, branch C only (CombatStyle.cpp:189-201). Leave branch A
(initial ownership, :165-176) and branch B (handoff, :184-188) intact — those are the
one-shot legitimate sets; gating them would change what OFF means. Concretely, at the
top of the `else if (a_cc->combatStyle != target)` body:

```
} else if (a_cc->combatStyle != target) {
    ++o.rederives;                                   // still COUNT the re-derive
    if (!Config::g_cstyReassert.load(std::memory_order_relaxed)) {
        // PROOF MODE: do NOT fight the engine. One-shot ownership stands
        // from branch A; the engine's re-derived style is left in place.
        return;
    }
    a_cc->combatStyle = target;                      // (existing re-assert)
    if (o.rederives == 1) spdlog::info(...);         // (existing log)
}
```

Counting the re-derive even when OFF keeps the `[wstyle]` state report meaningful
(marth can see "engine re-derived N times while re-assert was OFF" — confirms the engine
is still reverting, which is itself a finding).

Semantics of OFF: **set the stance style once when MFO takes the controller (branch A),
then never re-assert.** So OFF also directly tests the eventual fix (§4/§6): does the
one-shot set + the equip gate suffice, or does the engine immediately revert and the
archer redraw her bow? That dual purpose is deliberate.

### 2.3 Mid-battle flip mechanism
Config is parsed once at `kDataLoaded` (plugin.cpp:286 region); there is **no live INI
re-read path** (grep found none). `g_cstyReassert` is an atomic read every tick, so the
value takes effect instantly *if something flips it live*. Two options:

- **Preferred: a dev hotkey** mirroring the existing DIK-keycode handler at
  Board.cpp:1655-1671 (edge-only `b->IsDown()`, keyboard device, matching IDCode). Add a
  `bCstyReassert`-toggle key (e.g. `iCstyReassertKey`, an unbound DIK like the probe
  keys, Config.cpp:282,285) whose handler does a plain
  `g_cstyReassert.store(!g_cstyReassert.load())` — no `MainThread::Post` needed (it is
  a bare atomic, not engine mutation, unlike ProgProbe at Board.cpp:1669). This gives a
  true mid-battle A/B in one fight.
- **Fallback (no code beyond the toggle): save → edit INI → reload.** Slower but still
  one field session; acceptable if the hotkey is deemed scope creep.

**Recommendation:** ship the hotkey — it is ~15 lines against a proven pattern and is
the difference between "flip it mid-battle" (the ask) and "reload between A and B."

---

## 3. Regression verdict — did THIS batch amplify the re-assert? **YES, likely.**

The re-derive the loop fights is **event-driven off weapon/loadout/target churn**
(§1.3, CombatStyle.h:97-102). This batch added two churn sources that did not run
pre-batch:

### Facet #2 — weapon-order EQUIPMENT claim + force-equip (the strong amplifier)
- `EquipWeapon` force-equips the chosen weapon **with `forceEquip=true`** every time the
  equip rule wins (Actuation.cpp:895, `mgr->EquipObject(..., true, true)`), and on a
  category/base-mage swap it **force-UNEQUIPs the old lock then force-equips the new**
  (Actuation.cpp:893-895). A weapon change is a classic engine combat-style
  re-derivation trigger — this is precisely "under the swap" in the `[wstyle]` log line
  (CombatStyle.cpp:198).
- `ReconcileForcedWeapon` re-claims the equipment every hold tick (Actuation.cpp:1287),
  and the force-hold is designed to keep the weapon in-hand against the AI's re-arm —
  but the AI's re-arm attempt itself (denied by the equip gate, CombatStyle.cpp:252) is
  the "her own combat AI re-arms a spell, strips the forced weapon" window the header
  ties re-derivation to (CombatStyle.h:99-102). More force-equip activity → more
  re-derive episodes → branch C fires on more ticks.

### Facet #1 — melee followers now claim combat-target (secondary amplifier)
- `create=true` (Actuation.cpp:1012) means a pure-melee follower now gets a combat-target
  claim it did not get pre-batch, and the target redirect writes `currentCombatTarget`
  and `cc->targetHandle`/`previousTargetHandle` whenever the engine drifts
  (Targeting.cpp:101,118-119). A target change is a lighter but real re-derivation
  trigger (the AI re-scores its approach when the target moves). More followers now
  exercise this path in melee.

### Verdict
Pre-batch, a follower under an equip stance still hit branch C only when its own magic
re-arm churned the loadout. This batch **adds force-equip/unequip weapon swaps on the
hold path and target flips on the melee path**, both of which drive the exact
re-derivation branch C reacts to. So the batch did not create the loop, but it
**increased the rate at which the loop's condition fires** → effectively a regression in
the movement-smoothness dimension → strengthens "fix before release," at minimum ship
the proof toggle. (Confidence: medium-high on facet #2 as the dominant amplifier;
medium on facet #1. The toggle plus the `o.rederives` count will quantify it directly.)

---

## 4. The proper source-gate fix (own the style SELECTION)

Per gate-the-source, the elegant fix is to own the combat-style *selection* so the
engine never reverts MFO's choice — no flicker, no re-assert, no loop.

### 4.1 Is there a clean chainable vfunc for combat-style selection? **No — not today.**
The APMF RE research already answered this, and I found nothing to overturn it:
- **CHANNEL-MAP.md** has no combat-style-selection row; the closest, headtracking (#5),
  is unrelated.
- **`Docs/archive/MFO-CONVERSION-ROADMAP.md` row #5** (archived 2026-09-07) (facet #5, combat-style/CSTY bias) verdict:
  *"C — no matching APMF channel exists ... No APMF check anywhere in the CSTY-swap
  path ... Would need either a new 'combat-style claim' channel or a deliberate redesign
  onto ch.15."* And the sequencing note (roadmap lines 104-108): *"lowest urgency of the
  B/C items ... treat this as a design conversation, not a port ... revisit only after
  #2/#6."*
- **ALLOWANCE-TEMPLATE.md** enumerates every reversed decision seam (T1 combat-tree
  leaves, T2 CombatObject Check* family, T3 0x49 package-offer, T4 body-command). **None
  is a combat-style selector.** The style is chosen internally by the combat system and
  written to `CombatController::combatStyle` (0x38); there is no reversed
  per-selection vfunc (`GetCombatStyle`/style-derive is a non-virtual/internal path,
  the same class of gap the template flags for the locomotion planner and package
  procedures — gateable only at coarse points, ALLOWANCE-TEMPLATE.md:29).

I could not grep the pinned CommonLib `CombatController.h` locally (vcpkg fetches it only
during the CI build; no copy on disk). **Flagged uncertainty:** a dedicated RE spike
*might* find the internal function that writes `cc->combatStyle` and a version-robust
way to hook it (so a claim makes it write MFO's style / skip the revert). That is real
reverse-engineering work, not a known vtable slot, and is exactly the "new combat-style
claim channel" the roadmap defers. **Do not gate 2.0.0 on it.**

### 4.2 The adjacent source-gate that already exists — the equip gate (facet #2)
The reason MFO needs the CSTY bias at all is to stop the archer re-arming a spell / the
AI re-drawing the off-stance weapon (CombatStyle.h:6-13, 97-108). That prohibition is
**already implemented at the source**: `EquipGateThunk` (CombatStyle.cpp:252) hooks
`CheckShouldEquip` (vtable 0x0F) and answers NO for spell/staff re-arm while an equip
order owns the stance (CombatStyle.cpp:302-333). This is a clean input-gate — the AI
never gets the option, so there is nothing to revert to.

**The insight for the fix:** if the equip gate alone holds the weapon (it denies the
re-arm that drives the re-derivation), then the per-tick CSTY *re-assert* may be
redundant — a one-shot style set at controller-acquire (branch A) plus the gate could
suffice, dropping the loop entirely. **The A/B toggle at OFF tests exactly this**
(§2.2): OFF = one-shot set + gate, no re-assert. If positioning smooths AND the archer
still commits to melee (the gate holds), the "proper fix" for release is simply *do not
re-assert* — no new APMF channel required.

### 4.3 Redesign-onto-ch.15 (the heavier alternative, post-release)
The roadmap's other option: instead of biasing the style, **remove the off-stance
weapon** via ch.15 Equipment (unequip the bow so the AI has no ranged option and closes
natively). This is a genuine source-gate (deny the AI the input) but is heavier
(unequip/re-equip mid-combat, the MFO #62 off-main-equip scar, CHANNEL-MAP.md:48) and is
a design change, not a port. Post-release.

---

## 5. Cheapest CORRECT interim fix — assessed honestly

**Write-on-change only is ALREADY the current behavior, and it does NOT help.** Branch C
is guarded by `else if (a_cc->combatStyle != target)` (CombatStyle.cpp:189) — MFO
already writes only when the live style differs from desired. The problem is that **the
engine reverts every tick during an episode**, so "differs" is true every tick, so
write-on-change writes every tick. There is no cheaper mitigation hiding here; the
existing code is already the minimal write.

The only behavior-changing cheap options are:
- **Stop re-asserting** (branch C becomes a no-op / `return`) — this is the `bCstyReassert=OFF`
  path. It is cheap and correct *if* the equip gate holds the stance (§4.2). This is the
  leading candidate for the actual 2.0.0 fix, pending the A/B proof.
- **Re-assert with hysteresis** (only every N ticks, or only when the re-derived style is
  materially different) — reduces flicker frequency but trades feature strength for
  smoothness and does not remove the fight. Not recommended; it is a band-aid
  (contra marth's proper-solutions rule).

---

## 6. PROVE-THEN-FIX plan (recommendation for 2.0.0)

marth's stated rule: *"if we can prove it's the re-assert, we fix now."* So:

1. **BUILD FIRST: the proof toggle + hotkey** (§2). `bCstyReassert` default ON (zero
   behavior change shipped), gated on branch C only, flippable mid-battle via a dev
   hotkey mirroring Board.cpp:1655-1671. Small, safe, one CI cycle. Also fold the
   `o.rederives` count into the state report so the engine's revert rate is visible with
   the loop OFF.
2. **FIELD-PROVE in one session:** enter a fight with a follower under an equip/melee
   stance (an archer given `act.equip_melee` is the canonical repro, CombatStyle.h:6-11).
   Flip `bCstyReassert` OFF mid-battle.
   - **Positioning smooths + follower still commits to the weapon** → PROVEN: the
     re-assert causes the stutter AND the equip gate holds the stance without it. Ship
     the fix in the SAME 2.0.0 cycle: make branch C's re-assert default OFF (or delete
     the loop, keeping branch A one-shot + the gate). This is the clean release fix — no
     new APMF channel.
   - **Positioning smooths but the archer re-draws her bow / abandons melee** → the
     re-assert IS the cause of the stutter but the gate alone does not hold the stance.
     Ship the toggle OFF only if the stutter is worse than the occasional re-draw
     (marth's call); otherwise keep re-assert ON for 2.0.0 and schedule the true
     source-gate (§4.1 RE spike, or ch.15 redesign §4.3) post-release.
   - **Positioning does NOT smooth with re-assert OFF** → the re-assert is NOT the
     cause. Pivot to the force-equip weapon swap itself (facet #2, Actuation.cpp:893-895)
     and the target flip (facet #1) as the direct movement disruptor — test by gating
     those (e.g. bWeaponStyleControl OFF, Config.cpp:286) and re-observing.
3. **Do NOT go straight to the full source-gate.** It has no clean chainable hook today
   (§4.1) and is explicitly the lowest-urgency deferred item. The toggle is cheap, is
   the proof, and is very likely also the fix.

**One-line recommendation:** ship the `bCstyReassert` toggle + hotkey in 2.0.0, prove
in one session; the fix is almost certainly "stop re-asserting, let the equip gate hold
the stance," which is a one-flag change in the same cycle — not a new APMF channel.

---

## 7. SECONDARY — heal animation gap: clean fold-in, or hard deferred? **DEFER (both A and B).**

Context confirmed in code: **offense casts animate** because the follower's own AI fires
the gambit spell — the owned-cast branch equips the spell + claims cast/target facets +
Cast-biased style, no force (Actuation.cpp:504-537). That branch is **gated to Offense
only** (Actuation.cpp:504-509: requires `ClassifySpell == Offense`, `target != self`,
`target != player`). **Heals never take it** — they run through the direct path
(`CastSelfDirect`/`CastTargetDirect`, Actuation_Direct.cpp), which uses
`CastSpellImmediate` (kInstant), applies the effect directly, and **deliberately NEVER
equips the spell** (Actuation_Direct.cpp:19-24) — so no animation.

### 7.1 Approach A — widen the owned-cast (AI-fired) path to heals
Now that APMF's T2 `CheckCast` gate exists (deny all spells but the claimed one so the
AI can only fire the heal), could a heal go through the same animated AI-fired path?
- **The known blocker is still real.** `CastSelfDirect` / the direct path exist
  *because the vanilla AI will not reliably self/party-heal ON COMMAND* — a follower
  will not fire the exact heal MFO wants at the moment MFO wants it, even with a wounded
  ally present (Actuation_Direct.cpp:13-17, 49-61; memory `self-concentration-gambits-barred`
  → resolved *via CastSelfDirect*, i.e. by NOT relying on AI decision). The T2 gate can
  DENY every other spell, but **it cannot MANUFACTURE the heal decision** — that is the
  hard INVARIANT #0 line (APMF arbitrates/denies, never manufactures; CHANNEL-MAP.md:9-21,
  ALLOWANCE-TEMPLATE.md:10-16). Deny-all-but-heal makes the heal the *only* castable
  spell, but the AI still has to *choose to cast at all*; vanilla combat AI does not
  prioritize a commanded party-heal, so the follower may simply not cast — the exact gap
  CastSelfDirect was built to route around.
- **Verdict:** this is a **field-uncertain AI-behavior bet**, not a safe code change. It
  could be probed (deny-all + wounded ally, observe whether the AI fires the heal), but
  that is an experiment with an uncertain outcome and a regression risk (a heal that
  sometimes does not fire is worse than a guaranteed silent heal — "an unanimated heal
  beats no heal," Actuation.cpp:553-554). **Not a clean low-risk fold-in for 2.0.0.**

### 7.2 Approach B — animation overlay alongside CastSpellImmediate
Play a cast animation via `NotifyAnimationGraph` / `PlayIdle` so the direct heal LOOKS
animated while the effect still lands directly.
- **This is the hard deferred problem.** Memory `cast-animations-deferred-to-post-town-polish`
  records that a full sustained cast animation **could not be forced** this way — driving
  the caster state machine failed (pose lasted ~500ms then the spell unequipped, effect
  VFX lingered), and the animation R&D was explicitly stopped and deferred to a polish
  pass **after** the town update. The un-tried lever named there is exactly
  `NotifyAnimationGraph`/idle-play *separate from the magic apply* — i.e. Approach B is
  the *proposed* deferred experiment, not a proven low-risk technique.
- Memory `generalize-cast-anim-chain-all-forced-casts` slots the anim-chain generalization
  "right before town" and gates it on the self-cast animation chain being **field-proven
  first** — which it is not.
- **Verdict:** Approach B is **the hard deferred anim-forcing problem**, not a concrete
  low-risk overlay. Attempting it in the 2.0.0 CI/deploy would reopen R&D that was
  deliberately parked.

### 7.3 Secondary verdict
**Neither A nor B is a clean low-risk fold-in for the 2.0.0 CI cycle.** A is a
field-uncertain AI bet against the exact wall CastSelfDirect exists to avoid; B is the
known-hard, already-deferred animation-forcing problem. **Heal-anim stays an
early-post-release item** (the polish/anim-graph bucket), decoupled from the CSTY
movement fix. The CSTY toggle+fix (§1-6) is the primary 2.0.0 deliverable and does not
depend on this.

---

## 8. Files cited (all absolute)
- `/mnt/gaming/modlists/Projects/marth-follower-overhaul/native/CombatStyle.cpp` (ApplyTick :138, branches :165/:184/:189-201; equip gate :252-333; install :338)
- `/mnt/gaming/modlists/Projects/marth-follower-overhaul/native/CombatStyle.h` (:6-30 why-safe, :97-117 equip-gate rationale)
- `/mnt/gaming/modlists/Projects/marth-follower-overhaul/native/Targeting.cpp` (:44 original-first, :62 ApplyTick drive, :101/:118-119 target write)
- `/mnt/gaming/modlists/Projects/marth-follower-overhaul/native/Actuation.cpp` (:504-537 owned-cast Offense gate, :868-902 force-equip, :1010-1012 facet#1 create=true, :1249-1289 ReconcileForcedWeapon/ClaimEquipment)
- `/mnt/gaming/modlists/Projects/marth-follower-overhaul/native/Actuation_Direct.cpp` (:13-37 direct heal path, :19-24 never-equip)
- `/mnt/gaming/modlists/Projects/marth-follower-overhaul/native/Scheduler.cpp` (:766-824 stance decision + Want, :854-863 reconcile/release)
- `/mnt/gaming/modlists/Projects/marth-follower-overhaul/native/Config.h` (:244/:367 toggle pattern) / `Config.cpp` (:122-139 parse, :279-287 defaults)
- `/mnt/gaming/modlists/Projects/marth-follower-overhaul/native/Board.cpp` (:1655-1671 dev-hotkey pattern)
- `/mnt/gaming/modlists/Projects/ai-package-management-framework/Docs/CHANNEL-MAP.md` (facet rows, INVARIANTS #0)
- `/mnt/gaming/modlists/Projects/ai-package-management-framework/Docs/ALLOWANCE-TEMPLATE.md` (§1-4 the reversed seams; no CSTY selector)
- `/mnt/gaming/modlists/Projects/ai-package-management-framework/Docs/archive/MFO-CONVERSION-ROADMAP.md` (row #5 facet-#5 verdict, :104-108 sequencing)
</content>
</invoke>
