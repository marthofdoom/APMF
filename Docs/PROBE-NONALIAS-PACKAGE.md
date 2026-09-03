# Probe: is there a virtual choke point for NON-ALIAS package selection?

Status: **static research spike (2026-09-03), READ-ONLY, no code changes.** Answers the question APMF's
field symptom raised: MFO must re-assert its package on Cicero (framework package `0009BE51`, priority 60)
every tick instead of getting a quiet hold, because that package is almost certainly picked through the
engine's regular (non-alias) procedure-list path, which the existing 0x49 hook (`Actor::
CheckForCurrentAliasPackage`) does not govern.

**Verdict up front: no sibling virtual choke point exists in CommonLibSSE-NG's exposed headers that covers
the non-alias package pick.** `Actor::CheckForCurrentAliasPackage` (0x49) is the *only* package-selection
virtual on `TESObjectREFR`/`Actor`/`Character`. This corroborates — with direct header citations, not just
inference — `Docs/ALLOWANCE-TEMPLATE.md`'s existing "Honest gaps: package PROCEDURES (0x49 only)" line
(row 3, §4, §1 "NOT generic — package procedures"). The next-best options are laid out below, ranked by
plausibility, none of them clean.

## Source used

CommonLibSSE-NG headers do not exist locally in this sandbox (no vcpkg cache, no build tree — confirmed:
`find` for `Actor.h`, `CommonLibSSE*` under `/mnt/gaming/modlists/Projects` and `/` returned nothing except
`native/core/CombatBehaviorRE.h`'s own citation). `native/vcpkg-configuration.json` pins the `commonlibsse-ng`
port through the `colorglass/vcpkg-colorglass` registry at baseline `6309841a1ce770409708a67a9ba5c26c537d2937`.
`native/core/CombatBehaviorRE.h`'s own header comment (already in-tree) states the port this project actually
builds against resolves to CommonLibSSE-NG commit **`c4ab853d095e81e3390b282d7ba01ab2f24ebf25`**, and names
`CharmedBaryon/CommonLibSSE-NG` as the fork. All citations below were fetched live from
`https://raw.githubusercontent.com/CharmedBaryon/CommonLibSSE-NG/c4ab853d095e81e3390b282d7ba01ab2f24ebf25/...`
— i.e. the EXACT pinned commit, not `main`/HEAD. File tree existence was cross-checked via the GitHub API
tree listing at that same commit.

## 1. The 0x49 hook (template to mirror) — recap from in-repo evidence

`native/core/PackageGate.cpp:41-58` installs on `RE::VTABLE_Character[0]` slot `0x49`
(`PkgHook::idx = 0x49`), thunk calls the engine's own answer first (`func(a_this)`), only substitutes when
`ControlMap` has a live `kIntent_OfferPackage` claim naming a resolvable `TESPackage*` FormID, never invents
a null. Installed once at `native/core/PackageGate.cpp:62-76` (`packagegate::Install()`), VR-refused. Nudged
via `Actor::EvaluatePackage(actor, true, false)` (`RELOCATION_ID(36407, 37401)`,
`native/core/PackageGate.cpp:78-83`) from `native/channels/OfferPackage.cpp`'s `Engage`/`OnOwnerChanged`/
`Release` (game-thread only per `Channel.h`'s contract). `Docs/ALLOWANCE-TEMPLATE.md` §3's row "T3
package-offer" documents this as "Covers all package-driven activity (travel, sandbox, use-item,
activate-by-package, greeting)" — that line is the part this spike tests, and finds **overstated for the
non-alias procedure-list case** (see §5).

## 2. What CommonLib actually declares near 0x49 — exhaustive, by header

### `include/RE/T/TESObjectREFR.h` (base class), vtable slots 0x40–0x55 (fetched verbatim)

```
0x40  virtual void          UpdateSoundCallBack(bool a_endSceneAction);
0x41  virtual bool          SetDialogueWithPlayer(bool, bool, TESTopicInfo*);
0x42  virtual void          DamageObject(float, bool);
0x43  virtual bool          GetFullLODRef() const;
0x44  virtual void          SetFullLODRef(bool);
0x45  virtual BGSAnimationSequencer* GetSequencer() const;
0x46  virtual bool          QCanUpdateSync() const;                 // { return true; }
0x47  virtual bool          GetAllowPromoteToPersistent() const;    // { return true; }
0x48  virtual bool          HasKeywordHelper(const BGSKeyword*) const;
0x49  virtual TESPackage*   CheckForCurrentAliasPackage();          // { return 0; }   <-- the ONLY package-selection vfunc in this range
0x4A  virtual BGSScene*     GetCurrentScene() const;
0x4B  virtual void          SetCurrentScene(BGSScene*);
0x4C  virtual bool          UpdateInDialogue(DialogueResponse*, bool);
0x4D  virtual BGSDialogueBranch* GetExclusiveBranch() const;
0x4E  virtual void          SetExclusiveBranch(BGSDialogueBranch*);
0x4F  virtual void          PauseCurrentDialogue();
0x50  virtual void          SetActorCause(ActorCause*);
...
```

No package/procedure vfunc anywhere else in 0x40–0x55. `Actor.h` (the derived class, same slot numbering)
overrides only some of these (`0x41`, `0x45`, `0x48`, `0x49`, `0x4A`–`0x4F`) and confirms the SAME base
implementation for 0x49: `[[nodiscard]] TESPackage* CheckForCurrentAliasPackage() override; // 049 - { return 0; }`.
`Character.h` does **not** redeclare 0x49 at all — it inherits `Actor`'s override, consistent with
`PackageGate.cpp` hooking `VTABLE_Character[0]` (the primary/Actor-inherited vtable) at that slot.

### `include/RE/A/Actor.h`, package/procedure hits (full-file search)

| Symbol | Virtual? | Index | Notes |
|---|---|---|---|
| `CheckForCurrentAliasPackage()` | **yes** | `0x49` | alias-only, base impl `{ return 0; }` |
| `TESPackage* GetCurrentPackage()` / `const TESPackage* GetCurrentPackage() const` | **NO** | — | plain member function, no `override`, no index comment — cannot be vtable-hooked |
| `void PutCreatedPackage(TESPackage*, bool tempPackage, bool createdPackage, bool allowFromFurniture)` | **yes** | `0xDF` | see §3 — scoped to CREATED/temp packages |
| `bool InitiateTresPassPackage(TrespassPackage*)` | yes | `0xD7` | trespass-specific, `{ return 0; }` base |
| `void InitiateGetUpPackage()` | yes | `0xDE` | get-up-specific |
| `void InitiateFlee(...)` | yes | `0xDD` | flee-specific |
| `void EvaluatePackage(bool a_immediate = false, bool a_resetAI = false)` | **NO** | — | plain member; this is the AL-relocation function `PackageGate.cpp` already calls by address (`RELOCATION_ID(36407,37401)`), not a vtable slot |
| `TESPackage* unk150` | n/a (data) | `ACTOR_RUNTIME_DATA + 0x150` | a raw `TESPackage*` field, neighbor of `combatController` (`+0x158`, the field CLAUDE.md's #4 layout-bug boundary already tracks) — no accessor found that reads/writes it by name in the header |

### `include/RE/A/AIProcess.h`

No virtual functions at all (not a polymorphic class in the exposed headers — no vtable). Holds
`ActorPackage currentPackage;` at offset `0x18` as **plain data**. There is no virtual setter for this field
anywhere in the header; whatever internal engine routine writes it is not exposed as a `RE::` symbol at all.

### `include/RE/T/TESPackage.h`

Its own vtable (`IsActorAtLocation` 0x3B, `IsActorAtSecondLocation` 0x3C, `IsActorAtRefTarget` 0x3D,
`IsTargetAtLocation` 0x3E, `IsPackageOwner` 0x3F, plus the standard `TESForm` slots 0x00–0x39) are all
CONTENT queries the engine asks of a package that is *already* the active one (location/ownership checks
during package execution) — none of them is a "selection" seat; nothing to hook to influence which package
gets picked.

### No `ProcedureManager.h` / `PackageManager.h` in the tree

A full recursive tree listing of the pinned commit (`c4ab853d...`, via the GitHub API) turned up exactly
five paths matching `Procedure|PackageManager|ProcedureManager` (case-insensitive):
`Flash/AS2/CLIK/gfx/managers/PopUpManager.as` (irrelevant, Scaleform), and
`include/RE/B/BGSProcedureTreeBranch.h`, `BGSProcedureTreeConditionalItem.h`, `BGSProcedureTreeProcedure.h`,
`BGSProcedureTreeSequence.h`. **There is no `RE::ProcedureManager` or `RE::PackageManager` class exposed at
all** — the subsystem that actually walks an actor's package list, evaluates conditions, and assigns
`AIProcess::currentPackage` is entirely unreversed in this header set.

### `include/RE/B/BGSProcedureTreeProcedure.h`

Derives from `BGSTypedItem<BGSProcedureTreeProcedure, BGSProcedureTreeConditionalItem>` and DOES carry a real
vtable — 17 slots (`0x00` dtor through `0x10`) — but every slot past the destructor and `Load` (`0x03`) is an
unreversed placeholder: `void Unk_01()` … `void Unk_10()`, all `void(void)` signatures with no known real
arguments or semantics. This is structurally *exactly* where a per-procedure "is this procedure current /
should it activate" decision would live (mirrors `Docs/ALLOWANCE-TEMPLATE.md` §1's own framing: "no reversed
per-procedure Update/Execute exists in the tree"), but there is nothing here to hook safely — wrong signature
guesses on a blind vtable slot are an ABI mismatch, i.e. a crash, not a graceful no-op.

## 3. Ranked candidates

1. **`Actor::CheckForCurrentAliasPackage` (0x49)** — proven, already hooked, but literally alias-scoped:
   both `TESObjectREFR`'s and `Actor`'s base implementations return `0` unconditionally, and the name/doc
   context (`GetCurrentScene`/`SetCurrentScene`/dialogue-branch neighbors at 0x4A-0x4F) places it in the
   "scripted alias override" family, not the general procedure-list pick. **Does not cover 0009BE51-style
   authored/faction/quest-list packages picked through the normal path** (matches the field symptom).

2. **`Actor::PutCreatedPackage` (0xDF)** — the only OTHER package-shaped virtual on the whole `Actor`
   vtable. Takes the `TESPackage*` being installed as an argument plus `tempPackage`/`createdPackage`/
   `allowFromFurniture` flags — structurally a "here is the package I'm about to make current" seat, which is
   the right SHAPE for a T3 sibling. **But** its immediate neighbors are `InitiateTresPassPackage` (0xD7),
   `InitiateFlee` (0xDD), `InitiateGetUpPackage` (0xDE) — all special-circumstance package spawners
   (trespass response, flee, get-up-from-ragdoll). This strongly suggests `PutCreatedPackage` is the shared
   callee those `Initiate*` functions call to install their DYNAMICALLY CREATED package, not the seat the
   engine uses when switching to the next authored package in an actor's regular package-list/AI-package
   evaluation. **Unconfirmed either way from headers alone** — this is the one candidate worth a cheap
   runtime probe (see §6) before writing it off.

3. **`Actor::GetCurrentPackage()`** — reads as the obvious name match, but it is **not virtual** (no index
   comment, no `override` in either the const or non-const overload). Hooking it would require a non-virtual
   call-site/detour patch — explicitly the class of fix the brief bans (mirrors the T4
   `TESActionData::Process` callee-entry patch that field-CRASHED against SCAR.dll's own patch at the same
   address, `Docs/ALLOWANCE-TEMPLATE.md` §6). Not a candidate under the "virtual hook, chainable" constraint.

4. **`AIProcess::currentPackage`** — plain `ActorPackage` data member at `+0x18`, no virtual mutator exposed
   anywhere. The write path is internal, unreversed engine code (no `ProcedureManager`/`PackageManager`
   header exists in the pinned tree at all — confirmed by a full recursive listing, §2). Not hookable as a
   vtable seat; at best a poll/observe target (read after the fact), not a deny seat.

5. **`BGSProcedureTreeProcedure`'s own vtable (slots 0x01–0x10)** — the class is genuinely polymorphic and
   is structurally the closest thing to "one interface, per-procedure decision" (the T1 combat-behavior-tree
   pattern's package-side analogue), but every slot is an unreversed `Unk_XX` with no known signature. Hooking
   blind here has no RTTI-verified confidence and a wrong signature is silent ABI corruption, not a benign
   miss. **Not buildable today** without a separate, dedicated RE spike (disassemble a live procedure
   instance's vtable, name the real Update/Evaluate slot and its args) — this is its own project, not a quick
   sibling hook.

6. **Native deny bits** (`kMovementBlocked`, `SetDontMove`+`KeepOffsetFromActor(self,0)`, `SetActivationBlocked`)
   — already catalogued in `Docs/ALLOWANCE-TEMPLATE.md` §1/§3. These stop the EFFECTS of a package (movement,
   activation) but do not stop the engine from considering `0009BE51` "current" — not a quiet hold, just a
   different kind of fight (the package stays assigned, its locomotion is what gets blocked). Does not meet
   the "engine never hands the actor that package" bar the brief sets.

## 4. Answering the brief's core ask directly

**No, there is no clean version-robust virtual choke point, sibling to 0x49, that covers non-alias package
selection**, based on an exhaustive read of `TESObjectREFR`/`Actor`/`Character`/`AIProcess`/`TESPackage`/
`BGSProcedureTreeProcedure` at the exact pinned commit. The two structurally plausible candidates
(`PutCreatedPackage` 0xDF, `BGSProcedureTreeProcedure`'s `Unk_XX` slots) are each disqualified for a
different reason: 0xDF is scoped (by its own neighbor functions) to created/temporary packages, not the
regular procedure-list pick, and the procedure-tree vtable is real but entirely unreversed, so hooking it
would violate the "RTTI-verified, known signature" discipline the whole T1–T3 template exists to guarantee.
This is a direct, header-cited confirmation of `Docs/ALLOWANCE-TEMPLATE.md`'s own prior "Honest gaps: package
PROCEDURES (0x49 only)" line — not a new finding contradicting it, but the receipts for it.

## 5. A question worth asking before building anything: does 0x49 actually get called for Cicero at all?

Every base implementation of `CheckForCurrentAliasPackage` returns `0` unconditionally
(`TESObjectREFR.h:49`, `Actor.h:049`). That is consistent with either of two very different engine behaviors,
and headers alone can't distinguish them:

- **(A)** The engine's package-pick routine calls `CheckForCurrentAliasPackage()` on EVERY evaluation,
  regardless of whether the actor has any alias-package instances, and only *acts* on a non-null return.
  Under (A), APMF's hook genuinely fires for Cicero too (0x49 dispatches through the vtable "from several
  threads", per `PackageGate.cpp`'s header comment and Phase 0 field data) — the field symptom (re-assert
  loop, not a quiet hold) would then have a DIFFERENT root cause: something about how the return value is
  *consumed* for a non-alias-eligible actor (e.g. the outer routine only trusts a non-null 0x49 answer when
  its own `ExtraAliasInstanceArray`-presence precondition already passed, discarding the hook's answer
  otherwise) — not a missing hook location at all, but a missing PRECONDITION APMF can't influence via 0x49.
- **(B)** The engine's package-pick routine only calls `CheckForCurrentAliasPackage()` inside a branch gated
  on the actor already having alias-package machinery active (e.g. non-empty `ExtraAliasInstanceArray`), and
  never calls it at all on the plain procedure-list path Cicero's `0009BE51` goes through. Under (B), the
  vfunc genuinely never fires for Cicero, matching the brief's framing exactly, and no header-level fix
  exists — the real chokepoint is non-virtual engine code with no exposed `RE::` symbol.

**This spike cannot distinguish (A) from (B) from headers alone** — both are consistent with everything
found above. Resolving it needs either a disassembly trace of `CheckForCurrentAliasPackage`'s call site(s)
inside the compiled `SkyrimSE.exe`/`SkyrimAE.exe` (IDA/Ghidra, not available in this sandbox — no game
binary present, headers-only environment), or the cheap runtime probe below.

## 6. What a runtime probe should check (this is static research; nothing below was executed)

1. **Cheapest, do this first:** add OBSERVE-only logging (no denial, no return-value change — mirrors the
   proven `T1Probe`/`AliasPkgProbe` discipline from `Docs/PROBE-ALLOWANCE.md`) to the EXISTING 0x49 thunk:
   log `a_this->GetFormID()` and `orig` every time it fires, for a save with Cicero present, walking near him
   while his `0009BE51` package is active. If the thunk **never fires for Cicero's FormID** at all → hypothesis
   (B), confirms 0x49 truly excludes the non-alias path, and the "no virtual choke exists" verdict above
   stands as the final answer — the remaining options are the re-assert status quo, the 0xDF probe (below),
   or a from-scratch procedure-tree RE spike. If it **does fire for Cicero's FormID** → hypothesis (A), and
   the actual bug is downstream of the hook (worth its own separate investigation into what precondition the
   outer routine checks before trusting 0x49's answer).
2. **Second cheapest:** OBSERVE-only hook `PutCreatedPackage` (0xDF) the same way — log `a_this`, `a_package`
   FormID, and the three bools, for the same Cicero walk-around. If `0009BE51` (or any package matching
   Cicero's actual assigned package at the time) ever appears as `a_package` there, 0xDF is a real second
   attach point and can graduate to a T3-sibling gate using the exact same template (engine-answer-first is
   moot since it's void — the gate would instead conditionally rewrite `a_package` before forwarding to
   `orig`, or skip the call+substitute the claim's package via `EvaluatePackage`-style re-trigger). If it
   never fires for the regular pick, rule it out for good.
3. If both (1) and (2) come back negative for Cicero specifically, the honest conclusion is: **the non-alias
   procedure-list pick has no reachable virtual seat in current CommonLibSSE-NG**, and closing this gap
   requires either (a) staying on the current re-assert-per-tick approach (works, just not quiet), or (b) a
   dedicated binary-level RE spike to name `BGSProcedureTreeProcedure`'s real `Unk_XX` slots (own project,
   IDA/Ghidra against the live `.exe`, well beyond this header-only spike's scope).

## Evidence file index

- `native/core/PackageGate.cpp:41-83`, `native/core/PackageGate.h` — the 0x49 hook this spike used as the
  template to mirror.
- `native/channels/OfferPackage.cpp:30-68` — claim lifecycle / `EvaluatePackage` nudge call sites.
- `Docs/ALLOWANCE-TEMPLATE.md` §1 ("NOT generic — package procedures"), §2 row 4 (0x49), §3 T3 row, §4
  ("Honest gaps: package PROCEDURES (0x49 only)") — the prior research this spike corroborates with direct
  header citations rather than re-deriving from scratch.
- `native/vcpkg-configuration.json` — pins the `commonlibsse-ng` port via `colorglass/vcpkg-colorglass`.
- `native/core/CombatBehaviorRE.h` (header comment, lines ~1-20) — names the actual resolved CommonLib commit
  `c4ab853d095e81e3390b282d7ba01ab2f24ebf25` (`CharmedBaryon/CommonLibSSE-NG`) this project builds against;
  the source this spike fetched headers from.
- `https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/c4ab853d095e81e3390b282d7ba01ab2f24ebf25/include/RE/T/TESObjectREFR.h`
  (vtable 0x40-0x55), `.../include/RE/A/Actor.h` (0x49, 0xD7, 0xDF, `GetCurrentPackage`, `unk150`,
  `EvaluatePackage`, 0xD0-0x100 range), `.../include/RE/C/Character.h` (confirms no 0x49 redeclare),
  `.../include/RE/A/AIProcess.h` (`currentPackage` data member, no vtable), `.../include/RE/T/TESPackage.h`
  (own vtable, content-query only), `.../include/RE/B/BGSProcedureTreeProcedure.h` (17-slot vtable, all
  `Unk_XX` past `Load`).
