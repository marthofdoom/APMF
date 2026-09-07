> # ARCHIVED — SUPERSEDED, 2026-09-07
>
> **Superseded by MFO's `Docs/API-PORT-AUDIT.md` (2026-09-06), which surveys the same
> ground against a much later tree.** Moved to `Docs/archive/` on 2026-09-07. It was also
> never committed until then: this file lived as an untracked working-tree document from
> 2026-09-03 onward, so nothing in git recorded either its content or its staleness.
>
> What changed under it:
> - Row 4 (melee combat-target) — DONE (`API-PORT-AUDIT` row 8).
> - Row 6 (equip gate, B-item) — PORTED (`API-PORT-AUDIT` row 11, the
>   `IsEquipmentClaimActive` stand-down).
> - Row 7 "confirm on the next deck cycle" — that cycle RAN and did not confirm; see MFO's
>   `Docs/DIAG-2026-09-06-loot-travel.md`.
> - §3 item 0 (remove MFO's redundant 0x0A/0x0F hooks) — STILL NOT DONE. The double 0x0A
>   hook is live (`Docs/HOOK-SITE-COVERAGE.md` §6, `Docs/SPEC-GRADUATED-CAST.md` §3). That
>   one item is the only reason to read this file at all.
>
> It is an MFO-facing survey living in the APMF repo, which is the same misplacement flagged
> for `Docs/SPEC-COMBATSTYLE-SOURCEGATE.md`. Left here rather than moved across repos.

# MFO → APMF Conversion Roadmap

Research pass, 2026-09-03. Read-only survey of both repos (MFO
`/mnt/gaming/modlists/Projects/marth-follower-overhaul`, APMF
`/mnt/gaming/modlists/Projects/ai-package-management-framework`, both at their
working-tree HEAD — MFO on `feat/apmf-cast`, latest commit `f80021f`). Purpose:
enumerate every native control facet MFO owns, say which of them already route
through APMF and which don't, match each to an APMF channel (or note the
absence of one), and give each a readiness verdict so the next conversion pass
knows where to spend effort.

Guiding principle ([[mfo-is-apmf-showpiece-legacy-is-absent-only]]): with APMF
present, MFO should route control through APMF's channels; MFO's own hooks are
the APMF-ABSENT degrade only, never a silent decline-fallback.

**Verdict key**
- **A — CONFIRMED 1:1, ready now.** A proven APMF channel can own this
  behavior with no new APMF work; converting is wiring + standing the native
  path down.
- **B — needs APMF work first.** Names the specific missing APMF capability.
- **C — new capability, no existing MFO behavior.** The channel exists but
  nothing in MFO uses it today; defer until a feature needs it.

---

## 1. Ranked table

| # | MFO facet | MFO native mechanism (file:line) | Current APMF routing | Matching APMF channel + proof status | Verdict |
|---|-----------|-----------------------------------|-----------------------|----------------------------------------|---------|
| 1 | **Cast selection — EXACT (castLvl 4, hostile offense spell)** | `Actuation.cpp:504-537` (ownedCast branch); execution via `CasterConsent::Want` (`Actuation.cpp:460`), `Targeting::Command` (`:533`), Cast-biased combat style (Scheduler) | **FULL, but not exclusive yet.** `APMFBridge::ClaimCasting`/`ClaimCombatTarget` (`Actuation.cpp:522-523`); MFO's own exclusivity denies stand down via `IsOwnedCastActive` (`CasterConsent.cpp:572,843`; `CombatStyle.cpp:284`) — but MFO's `CheckCast`/`CheckShouldEquip` hooks (`CasterConsent.cpp`'s `CheckCast` install; `CombatStyle.cpp`'s equip gate) are **still physically installed on the same two vtable slots** APMF's T2c/T2a hook (0x0A, 0x0F) — a documented double-hook, not yet removed | ch.8 CastingSelect (arbitration, `channels/CastingSelect.cpp`) + T2c CastGate (`core/CastGate.cpp`, hooks `MagicCaster::CheckCast` slot 0x0A) + T2a EquipGate (`core/EquipGate.cpp`, slot 0x0F, commit `11611d3`) — **PROVEN for the arbitration/stand-down mechanism** (ALLOWANCE-TEMPLATE.md §7.2), but the T2c/T2a *hook code itself* is uncorroborated in the field: commit `11611d3`'s own message says "threading-sensitive framework code; not deployed pending review," and `HOOK-SITE-COVERAGE.md:294-299` states explicitly that "rows 2/8 and 3/9 are the same CommonLib slots hooked twice, once by APMF ... and once by MFO ... this is exactly what `feat/apmf-cast` commit `f80021f` ... is in the middle of resolving — not a CommonLib gap, just noted here so the two-repo hook inventory is honest about the current overlap while that migration is mid-flight." `STATUS.md` has no entry for T2c/T2a at all (undocumented since the commit) | **A — mechanism proven, cleanup unfinished.** The arbitration/stand-down design is confirmed correct and is MFO's ship record; what's NOT done is dropping MFO's now-redundant `CheckCast`/`CheckShouldEquip` native hooks (`CasterConsent.cpp`, `CombatStyle.cpp`) now that APMF's own T2c/T2a enforce the same slots. This is pure cleanup (delete the now-provably-redundant native enforcement), not new design work — but it's a real open item, not "nothing left to do." Also confirm APMF's T2c/T2a have had their own deck pass before treating this as fully proven end-to-end (commit says "not deployed pending review"). |
| 2 | **Cast selection — GRADUATED (castLvl 1-3: exempt buffs/heals by category) + self/player-targeted heals** | `CasterConsent.cpp:112-118` (`CastExempt`, 4-level slider), `:206-229` (`CtrlUnlatchedDeny`), `:826-829` (`ConcUnboundedDeny`, hard concentration time-bound — runs unconditionally, never stands down for APMF), `:540-593` legacy AI-first-wait + `ForceCast` hybrid in `Actuation.cpp`; self-casts and target-concentration additionally bypass `CasterConsent`'s AI-deliberation path entirely via `CastSelfDirect`/`CastTargetDirect` (`Actuation_Direct.cpp:729,927`, direct `CastSpellImmediate` — never touches `CheckStartCast`/`CheckCast` at all, confirmed `Actuation.cpp:211-220`) | **NONE.** `ownedCast` requires `ClassifySpell()==Offense && target!=self && target!=player` (`Actuation.cpp:504-509`) — self-heals, buffs, and castLvl<4 partial exemptions never reach the owned-cast branch and run 100% native even with APMF present. The concentration hard-bound (`ConcUnboundedDeny`) is a safety rule that stays native by design regardless of APMF, and is a separate concern from the exemption-slider gap | T2c CastGate's `Allowed()` (`core/Allowance.h:86`) compares the claim's **single** `param.form` against the deliberated spell — no category/list concept exists in `ControlMap::Claim` (`ControlMap.h:113-117`, one `APMF_Param` per claim); confirmed **binary, exact-match-only** (`Allowance.cpp:60-65`: `claim.form == subjectForm`) | **B — needs APMF work.** Missing: a category-or-list-aware `SelectSpell` claim (e.g. an exempt-kind bitmask analogous to ch.7's already-proven `CombatActionCategory` bitmask pattern, `ActionGate.cpp:36-51`, or multiple allowed forms in one claim) to express castLvl 1-3; plus MFO would need to widen `ownedCast`'s target/kind guard to admit self/party-heals once APMF can express the exemption. The `CastSelfDirect`/`CastTargetDirect` bypass path is a separate, deliberate design (skips AI deliberation altogether) and would need its own decision about whether APMF should arbitrate it at all, not just a bigger claim shape. |
| 3 | **Combat-target claim — caster path** | `Targeting.cpp` (`UpdateCombat` hook, installed `plugin.cpp:295`) executes the write unconditionally; `Actuation.cpp:523,1011` claim it | **FULL** for any follower that has an active or prior-this-fight cast claim | ch.6 CombatTarget (arbitration-only, `channels/CombatTarget.cpp`) — **DOCUMENTED**, arbitration-only by design (`CHANNEL-MAP.md` row 6); APMF makes no engine write here by design, so "proof" = the claim bookkeeping, which is exercised live by facet 1 | **A — shipped** as part of facet 1. |
| 4 | **Combat-target claim — pure-melee attack directives (no cast involved)** | Same `Targeting::Command` call (`Actuation.cpp:1029`), but `ClaimCombatTarget(..., create=false)` (`:1009-1011`) — a pure-melee follower is deliberately **never** claimed | **NONE.** A melee-only follower's target write is 100% native, zero APMF arbitration, by explicit design choice (`Actuation.cpp:1007` comment: "create=false ... NEVER creates a claim for a pure-melee follower") | Same ch.6 CombatTarget channel; no new capability needed | **A — ready now, trivial.** The mechanism is already proven for casters; extending `create=true` to melee-attack directives (or a considered subset) is a wiring decision in `Actuation.cpp`, not new APMF work. Flagging because it's the cheapest open conversion in this table. |
| 5 | **Weapon-equip-order combat-STYLE bias (CSTY swap: Melee/Ranged/Cast)** | `CombatStyle.h:32-94`, `CombatStyle.cpp` `Want`/`ApplyTick`, driven every combat tick from `Targeting.cpp:62` inside the `UpdateCombat` hook | **NONE.** No APMF check anywhere in the CSTY-swap path; runs unconditionally regardless of APMF presence | No direct equivalent. ch.3 Stance (`channels/Stance.cpp`) is sneak/crouch only — unrelated concept. ch.15 Equipment (`channels/Equipment.cpp`) is the documented "melee-vs-ranged lever" but achieves it by **removing** a weapon, not by biasing the AI's own combat-style weighting — a different mechanism for a related problem | **C — no matching APMF channel exists.** ch.15 is an adjacent tool (could replace this facet with a design change: unequip the off-style weapon instead of biasing CSTY) but is not a 1:1 port. Would need either a new "combat-style claim" channel or a deliberate redesign onto ch.15. |
| 6 | **Weapon-equip-order EQUIP GATE (deny AI re-arm of spell/staff while a weapon-order owns the hands)** | `CombatStyle.cpp:236-318` (`EquipGateThunk`, hooks `CheckShouldEquip` slot 0x0F on 30 caster-category vtables), installed `plugin.cpp:297` | **PARTIAL.** Stands down only when the **same follower's cast** is APMF-owned (`CombatStyle.cpp:284`, `IsOwnedCastActive(fid)`) — i.e. only in the narrow overlap case. The common case (an `act.equip_melee`/`ranged` gambit with no live cast claim on that follower) runs 100% native regardless of APMF | T2a EquipGate (`core/EquipGate.cpp`) exists and is **PROVEN** for the cast facet (`Allowed(fid, kIntent_SelectSpell, ...)`, `EquipGate.cpp:88`) but is scoped to `kIntent_SelectSpell` only — it has no notion of a weapon-stance-order facet to gate against | **B — needs APMF work.** Would need T2a's `Allowed()` check extended to also honor a `kIntent_Equipment` claim (deny spell/staff re-arm while an Equipment claim holds the weapon slot), or a new facet. ch.15's `Engage`/`Release` today only does a one-shot unequip/re-equip — no persistent `CheckShouldEquip` deny of the AI's own re-arm attempts. |
| 7 | **Loot-travel package offer** | `Packages.cpp` multiple call sites (`:1209,1395,1553,1632,1719,1771`) | **FULL, committed (no fallback).** `APMFBridge::OfferPackage`/`ReleaseOfferPackage` called at every loot-travel excursion start/end; `APMFBridge.h:124-129` explicitly frames this as a committed path, not try-then-decline | ch.9 OfferPackage (arbitration, `channels/OfferPackage.cpp`) + T3 PackageGate (`core/PackageGate.cpp`, hooks `CheckForCurrentAliasPackage` slot 0x49) — underlying **0x49 mechanism field-PROVEN** Phases 1-2 (`CHANNEL-MAP.md` row 9); **but** the graduated real-channel code itself (as of today's 2026-09-03 graduation pass) is flagged "**Not yet field-tested** (built + CI-green only)" in `STATUS.md:211-212`. Save/load (Phase 3) unexercised per `CHANNEL-MAP.md:72-73`. | **A — shipped**, with one caveat worth flagging to marth: the channel *code* backing this already-committed MFO path was reshaped today (probe → real API channel) and per APMF's own STATUS.md hasn't had its own deck pass yet. Confirm on the next field cycle before calling it fully proven end-to-end. |
| 8 | **Combat-action deny (suppress a follower's own chosen attack/cast leaf by category)** | None. Grepped `Actuation.cpp`/`CasterConsent.cpp`/`CombatStyle.cpp` for any "hold fire"/suppress-own-action gambit — none exists; no `act.*` gambit in MFO's vocabulary currently wants this | **Built, unwired.** `APMFBridge::ClaimCombatActionDeny`/`ReleaseCombatActionDeny` exist (`APMFBridge.h:138-154`) and compile, but have **zero call sites** outside `APMFBridge.cpp` anywhere in `native/*.cpp`. The header itself says why: "NOT wired into the loot-travel dispatch by default ... MFO's own PACKAGE-THEFT guard already concedes loot-travel to a live combat package on purpose" (`APMFBridge.h:144-147`) | ch.7 CombatAction (`channels/CombatAction.cpp`) + T1 ActionGate (`core/ActionGate.cpp`) — mechanism field-PROVEN via the T1 probe (`PROBE-ALLOWANCE.md`), graduated to a real channel 2026-09-03; category classification currently covers only `kCombatActionCat_Offense` (`ActionGate.cpp:61-74`) | **C — this is the canonical example.** No MFO gambit needs it today. Matches [[assassin-stealth-update-backlog]]: a future Rogue/assassin "hold fire"/"don't engage" gambit is the natural first caller. Defer until that feature lands. |
| 9 | **Movement stop / hold-position** | **None exists.** MFO has no native movement-block mechanism at all — `Probe.cpp:41-42` explicitly records `KeepOffsetFromActor`/`SetDontMove` as "Papyrus-only; no C++ binding in NG." The owned-cast model *deliberately* never claims movement (`Actuation.cpp:494`, "we do NOT claim the movement facet... so that granular non-interruption is exactly why the cast routes through APMF") | N/A | ch.1 Movement (full block, `KeepOffsetFromActor`+`SetDontMove`, Address-Library bound) — **DOCUMENTED/built**, PROMOTE feed is a documented GAP (`CHANNEL-MAP.md` row 1) | **C — unclaimed territory, matches [[next-main-goal-town-errands]].** Town-nav / "stay put" gambits are the plausible future caller; no current MFO behavior to convert. |
| 10 | **AI-attribute bias (aggression/confidence/morality/assistance)** | **None.** Grepped for `kAggression`/`kConfidence`/`kMorality`/`kAssistance`/`SetActorValue`/`ModActorValue` across `native/*.cpp` — zero hits. MFO's own `Confidence.h` is an unrelated internal 0-1 "how safe do I feel" scalar driving the loot leash, not the engine AV | N/A | ch.11 Disposition (`channels/Attribute.cpp`) — **DOCUMENTED, "cleanest gate on the board"** per `CHANNEL-MAP.md` row 11, co-saved/save-safe | **C — unused today.** Genuinely orthogonal to MFO's existing `Confidence.h` concept; would be new gameplay (temperament-driven aggression bias), not a port of anything existing. |
| 11 | **Headtrack, Sneak/Stance, WeaponDraw, Dialogue, Gait-AV, Detection/stealth-AV, Idle, ShoutPower** | **None.** No native equivalents found in MFO for any of these (grepped `Shout`/`EquipShout`/`SelectedPower` — zero hits; MFO's own `Gait.cpp` is package-travel-speed, unrelated to the `kSpeedMult` AV ch.1a uses) | N/A | ch.5 Headtrack, ch.3 Stance-sneak, ch.4 WeaponDraw, ch.10 Dialogue, ch.1a Gait, ch.16 Detection, ch.12 Idle, ch.14 ShoutPower — all **DOCUMENTED/built** per `CHANNEL-MAP.md` | **C — all unused today.** Listed for completeness; none currently gate an MFO behavior. |

---

## 2. Notes on things that look converted but carry an asterisk

- **Cast selection — exact (#1) is not fully cut over yet, despite shipping.**
  `f80021f`'s own commit title ("owned cast becomes a pure APMF T2 client --
  drop redundant enforcement") states the intent, but `HOOK-SITE-COVERAGE.md:
  294-299` documents that MFO's native `CheckCast` (`CasterConsent.cpp`) and
  `CheckShouldEquip` (`CombatStyle.cpp`) hooks are **still physically
  installed** on the same two vtable slots (0x0A, 0x0F) APMF's T2c/T2a now
  also hook, and calls f80021f "in the middle of resolving" this, not
  finished. Separately, APMF's own T2c/T2a hook code (commit `11611d3`) is
  self-described as "not deployed pending review," and `STATUS.md` has no
  field-test entry for it. Two independent verification gaps stack here:
  (a) MFO's redundant native hooks aren't actually removed yet, and (b)
  APMF's replacement hooks haven't had their own deck confirmation. Neither
  blocks calling this facet's *design* an A, but both should be closed out
  before treating facet #1 as fully retired.
- **Combat-target for melee (#4)** is the cheapest real conversion on this
  table: it needs no new APMF capability, just flipping `create=false` →
  `create=true` (or a considered subset) at `Actuation.cpp:1009-1011` for
  non-cast targeting directives. Worth doing alongside whatever picks up #2 or
  #6, since it's a one-line-shape change riding infrastructure already proven
  by facet 1.
- **Loot-travel (#7)** is functionally "done" — it's MFO's only path, no
  fallback — but the underlying channel code was reshaped today
  (`ch.9`/`ch.7` graduation, `STATUS.md` "Not yet field-tested"). Not a
  blocker, just something to confirm on the next deck cycle rather than assume
  proven because MFO already ships it.
- **The equip-gate stand-down (#6)** only fires in the narrow case where the
  *same* follower simultaneously holds a live cast claim — which is rare,
  since equip-order gambits (weapon force) and cast gambits are different
  gambit types. For the common weapon-order case, MFO's own gate is the only
  enforcement even with APMF present. This is the most consequential B-item:
  it's actively running redundant/independent logic today, not just a gap.

## 3. Prioritized recommendation

0. **Close out facet #1's cleanup** — before anything else, confirm APMF's
   T2c/T2a (`CastGate.cpp`/`EquipGate.cpp`) have had a deck pass (commit
   `11611d3` says "not deployed pending review"), then remove MFO's now-
   redundant native `CheckCast`/`CheckShouldEquip` hooks per
   `HOOK-SITE-COVERAGE.md:294-299`. This finishes what `f80021f` already
   announced as done; leaving both hooks installed on the same slots is the
   kind of double-enforcement this whole migration exists to eliminate.
1. **Facet #4 (melee combat-target claiming)** — do this first among *new*
   conversions. Zero new APMF work, reuses facet 1's proven plumbing, closes
   the one place a pure-melee follower's target write has no arbitration
   record at all.
2. **Facet #6 (equip-gate for weapon-orders)** — highest-value B item. It's
   the one case where MFO is running a second, independently-configured deny
   mechanism in parallel with APMF's philosophy even when APMF is present
   (exactly the redundant-enforcement pattern facet #1/f80021f already fixed
   for the cast side). Needs APMF work first: extend T2a's `Allowed()` to
   also check a `kIntent_Equipment` claim, or land a dedicated facet.
3. **Facet #2 (graduated cast consent, castLvl 1-3 + self-heals)** — the
   biggest single behavioral gap (three of MFO's five cast-control levels
   still run natively), but it needs real APMF design work first: today's
   `SelectSpell` claim can only name one exact spell, and MFO's slider needs
   either a category-exemption param or a multi-form claim. Sequence this
   after #6 since both need a *shape* decision on `APMF_Param`/`ControlMap`
   for how a claim expresses "more than one form."
4. **Facet #5 (combat-style/CSTY bias)** — lowest urgency of the B/C items
   with real MFO behavior behind it. No existing APMF channel is a genuine
   match (ch.15 solves an adjacent problem differently); treat this as a
   design conversation, not a port, and revisit only after #2/#6 prove out
   the claim-shape questions those need anyway.
5. **Facets #8/#9/#10/#11 (CombatActionDeny, Movement, Disposition, and the
   rest)** — leave as-is. All are genuine C: proven or built APMF capability,
   zero present-day MFO behavior to attach it to. Pick these up when a
   concrete feature needs them (assassin/stealth gambits for #8, town-nav for
   #9), not before.
