# Allowance-template field probes — T1, 0x49 redirect, native-bit

Status: **throwaway instrumentation, field-test-first.** Every probe here is hotkey-
driven, observe-first, NOT wired to any client, and NOT a permanent channel. They exist
to answer the open questions in `Docs/ALLOWANCE-TEMPLATE.md` §5/§6 with LIVE data before
anything is built on top. Read that doc first — this one is the field-test companion:
hotkeys, exactly what each phase proves, expected-vs-actual (actual left blank for the
field run), and pass/fail. Update the ACTUAL column after each field session; keep
`ALLOWANCE-TEMPLATE.md` §6 in sync if a probe's findings firm up the design.

**T4 (`TESActionData::Process`) was built, field-crashed, and REMOVED (2026-09-03) —
see the "T4 — DEFERRED" section at the end for the crash record.** Three probes remain
active: T1, the 0x49 redirect, and native-bit.

**FIELD RESULTS (2026-09-03, deck, 1.6.1170):** T1 observe **PROVEN**, T1 deny
**PROVEN**, the `+0x158` ambiguity **RESOLVED (hypothesis B)**, 0x49 redirect
**PROVEN for Phases 1-2**. Native-bit untested this run. T4 stays removed (SCAR
collision, adapts to T1). Full detail + exact numbers in each probe's section below.

All three probes: VR-refused where the underlying vtable/RTTI/reloc index is SE/AE-only
verified (T1, 0x49 — the native-bit probe has no such dependency and runs on VR too),
install-once, lock-free reads only (no mutex in a hot thunk), warn-once/cadence-guarded
logging (first-hit-per-item + a ~5s periodic census, never per-call), and every
`CombatController`-shaped read stays `< 0x68` (the AE +8 spin-lock layout boundary,
CLAUDE.md #4 / ALLOWANCE-TEMPLATE.md §5).

## Hotkey map

**Standing convention: probe/test hotkeys use the NUMPAD.** F-keys are unusable here —
not a Deck-reachability issue, they're simply occupied by the game/modlist already.
Every future probe set should default to numpad without re-litigating this.

Consolidated to the fewest keys: ONE shared claim key drives T1 + the 0x49 redirect at
once (aim an NPC, press it — both claim the same actor in lockstep, since each
independently applies the same toggle logic to the same scancode), plus one dedicated
toggle key each for T1's Phase-1 deny and the two native-bit flags. The channel-demo
numpad keys are not needed for this pass and are freely overridden.

| Key | DIK | Probe | Action |
|---|---|---|---|
| NumpadEnter | 0x9C | T1 + 0x49 redirect (SHARED) | Claim/release the crosshair-AIMED NPC — one press claims it for T1 Phase 0 observe AND the 0x49 package-offer engage, both at once; press again (regardless of current aim) to release both |
| Numpad3 | 0x51 | T1 + 0x49 redirect (SHARED) | **No aim needed:** claim/release the NEAREST NPC currently IN COMBAT to the player, any allegiance (follower or enemy, whoever's fighting nearest) — same shared claim as NumpadEnter, so T1 observe + the 0x49 redirect both apply; refuses (logs, no-op) if nothing nearby is in combat, never claims a random calm NPC |
| NumpadSlash | 0xB5 | T1 | Toggle Phase 1 DENY (the `CombatBehaviorAttack` leaf only) on the claimed NPC — refuses if nothing is claimed |
| Numpad1 | 0x4F | Native-bit | Toggle `kAttackingDisabled` on the crosshair-aimed NPC |
| Numpad2 | 0x50 | Native-bit | Toggle `kCastingDisabled` on the crosshair-aimed NPC |
| Numpad0 | 0x52 | (test surface) | Release ALL controlled NPCs — unrelated to these probes, listed for collision-avoidance |

NumpadEnter/Numpad3 must be pressed before NumpadSlash (NumpadSlash refuses without a
live T1 claim). The native-bit probe has no claim step — Numpad1/Numpad2 act on
whatever the crosshair is aimed at on that press, independent of any claim. Both
claim-based probes (T1, 0x49) release their claim (no engine call, nothing to restore)
on `kPreLoadGame`. Numpad3 is the one-key battle-testing shortcut: walk into a fight,
press it, the nearest combatant is claimed with no aiming required.

## Probe 1 — T1: combat behavior-tree leaf `Enter`/`act`

**Files:** `native/core/CombatBehaviorRE.h` (the local `RE::CombatBehaviorTreeNode` /
`CombatBehaviorTreeControl` extension — CommonLibSSE-NG does not ship these classes;
see the file's header comment for the exact provenance of every VariantID triple),
`native/core/T1Probe.{h,cpp}`.

**Mechanism:** one thunk installed at vtable slot `0x02` (`act`, CPR's name for what
`ALLOWANCE-TEMPLATE.md` calls "Enter") on all 70 `VTABLE_CombatBehaviorTreeNodeObject_*`
leaf vtables, RTTI-verified against `RTTI_CombatBehaviorTreeNode` at install (the
`DerivesFrom` walk in `core/Allowance.h` — the `CombatMagicCasterArmor` lesson,
ENGINE_NOTES §0.28).

**Phase 0 (OBSERVE):** the thunk ALWAYS calls the leaf's original `act()` and returns
its result unmodified. When a claimed actor (NumpadEnter) is the one deliberating, it logs the
FIRST time each leaf fires (both our compile-time leaf name and the object's own
`GetName()`, an unhooked slot-1 virtual) and increments silent counters; a ~5s census
prints totals. This proves vtable dispatch actually works on this build and maps which
leaves fire in a real fight.

**The `+0x158` ambiguity — RESOLVED (2026-09-03, hypothesis B).** `act()`'s argument
(`CombatBehaviorTreeControl* control`, what CommonLib's own unshipped internals
apparently call `CombatBehaviorThread`) carries a `master_controller` pointer at
`+0x158`. Two readings existed:
- **Hypothesis A (CPR's own struct):** `+0x158` IS a `CombatController*` directly;
  `attackerHandle` sits at `+0x28` of THAT.
- **Hypothesis B (the doc's own noted alternative):** `+0x158` is a
  `CombatBehaviorController*`; its OWN `+0x20` holds the real `CombatController*`,
  whose `+0x28` is `attackerHandle`.

The thunk resolves and logs BOTH on the first hit. **Field result on this runtime
(1.6.1170): hypothesis A resolves NULL every time; hypothesis B is the correct
reading** — `control+0x158` is a `CombatBehaviorController*`, and its `+0x20` holds
the real `CombatController*` (`attackerHandle` at `+0x28` of that). CPR's own struct
(hypothesis A) was the wrong prior for this build despite being the stronger-looking
source (a compiled, shipping mod) — a reminder that a working mod's own header isn't
proof its layout matches every runtime. **Both the observe gate and the Phase-1 deny
guard already check `fidA == claim || fidB == claim` (never hypothesis A alone), so
this resolution required NO code change** — the template was already robust to
whichever hypothesis turns out correct, and would stay robust if a future runtime
flips which one resolves.

**Phase 1 (DENY):** NumpadSlash denies ONLY the `CombatBehaviorAttack` leaf for the claimed
actor via the engine's own failure protocol, `CombatBehaviorTreeControl::SetFailed(true)`
— never calling `orig()` for that hit.

**Field-crashed once, redesigned (2026-09-03).** The first build derived `SetFailed`'s
address by disassembling `CombatBehaviorForceFail`'s original `act()` body for its
first `0xE8` CALL, then hand-called that address as `void(CombatBehaviorTreeControl*,
bool)` (CPR's own declared signature). The crashlog confirmed the DERIVED ADDRESS was
correct (`module+0x5572A0`, independently verified) but the call still faulted inside
`SetFailed` (`mov [rdi],rbx` with `rdi=0x1`). Rather than re-guess the hand-rolled
signature a second time, the fix removes the reconstruction entirely: `T1Probe.cpp`
now invokes `CombatBehaviorForceFail`'s own ORIGINAL, unmodified `act()`
implementation directly — the SAME 2-arg `(leaf-this, control) -> control` convention
already proven correct for every `orig()` call in the file — instead of hand-calling
`SetFailed`. `ForceFail`'s compiled body performs `control->SetFailed(true); return
control` (`ALLOWANCE-TEMPLATE.md` §2 item 1) using whatever real calling convention
the compiler actually generated for that call; nothing is left to reconstruct. `this`
in that call is the Attack leaf's own object (not a real `ForceFail` instance), which
is safe because `ForceFail`'s body needs `this` for nothing — only `control`, which it
already receives directly. `SetFailed`'s own address is still extracted and logged as
a diagnostic cross-check (SE only, against CPR's documented `SkyrimSE.exe+0x7C6D30`)
but is never called directly anymore.

**Guard (`Docs/INVARIANTS.md` #17, adapt/degrade-never-crash), fixed twice
(2026-09-03):** the deny call requires `control`'s `+0x158` actor-resolution to have
matched the claimed actor before touching `control` for a write. The FIRST version of
this guard narrowed the check to hypothesis A alone ("the CPR-backed reading") —
field data showed observe (which accepts EITHER hypothesis, `fidA == claim || fidB ==
claim`) reliably resolved the correct actor on every hit, while hypothesis A alone
did NOT, so the deny guard failed every single time and never fired (census showed
`attackDenied=0`) even though the actor genuinely WAS resolvable. Fixed to use
`fidA == claim || fidB == claim` — the EXACT same condition observe already proved
reliable (it's the same expression the earlier "not our claimed actor" early-return
already checks; the deny guard now checks nothing narrower than what observe already
demonstrated). If NEITHER hypothesis resolves on a given hit, the deny is skipped
(warn-once) and the hit falls through to a plain observe instead — never an
unverified call.

**Expected vs actual:**

| Check | Expected | Actual |
|---|---|---|
| Any leaf fires at all | ≥1 leaf logs FIRST FIRE within a few seconds of combat | **PROVEN.** ~70 leaves fire through the vtable for real actors — logged: Circle, Bash, BlockAttack, Fallback, Attack, CastImmediate/CastConcentration, EquipSpell, Advance, SpecialAttack, Reposition, and the rest of the catalog — on Jesper (`0x750012C6`) and Cicero (`0x0009BCB0`). Vtable dispatch + RTTI derivation confirmed on this build. |
| `ForceFail::act()` deny mechanism resolved | logs a non-zero address at ARMED | Resolved (non-zero, logged at ARMED); the deny fires from it (see below), which is the strongest confirmation available. |
| `SetFailed` diagnostic address | resolves to a non-zero address (cross-check informational only) | Resolved and logged (informational; never called directly — see the redesign above). |
| Actor resolves for the deny (either hypothesis) | `fidA == claim \|\| fidB == claim`, matching observe | **Via hypothesis B specifically** (`+0x158` ambiguity resolved above) — hypothesis A resolves NULL on this runtime; the `A \|\| B` guard is exactly why this needed no further fix once the guard itself was corrected to match observe. |
| Deny actually fires | census `attackDenied > 0` after arming Phase 1 in a fight | **PROVEN.** `attackDenied=19` (Vampire Fledgling), `attackDenied=29` (Cicero) — 0 before the guard fix (narrowed-to-hypothesis-A bug), nonzero and climbing after it. Count freezes cleanly on Phase-1 disable. |
| Attack-leaf deny | tree falls back cleanly (block/circle/other leaf), NO CTD | Confirmed clean — no CTD across both field sessions (the crash session and the fixed re-test). |
| Deny side effects | no stutter, no re-entry storm (repeated `Enter` on Attack within ms) | None observed; census counts climb at a normal per-decision cadence, not a tight re-entry loop. |
| Guard skip (if it happens) | one warn-once log, falls back to observe, no crash | Not exercised as a skip in the passing run (the guard passed every hit once fixed to `A \|\| B`) — the earlier (buggy, hypothesis-A-only) build DID exercise this path repeatedly and it held with no crash, which is itself confirmation the skip-and-log path is safe. |

**Pass/fail: T1 PASSES — both observe and deny are PROVEN.** Leaves fire through the
installed vtables (dispatch + RTTI derivation real on this build) AND the Attack-leaf
deny fires and falls back cleanly with no stutter/re-entry storm/CTD.

## Probe 2 — 0x49 package-offer REDIRECT (Phases 1-3)

**Files:** `native/core/AliasPkgProbe.{h,cpp}` (extends the existing Phase-0-proven
probe; Phase 0 itself — does 0x49 fire at all — is unchanged and already LIVE per
`Docs/STATUS.md`).

**The offered package:** `DefaultSandboxCurrentLocation256` — Skyrim.esm, FormID
`0x000956B8`. Found by parsing Skyrim.esm's own `PACK` record group directly (a small
Python script reading the TES4 GRUP/record structure, decompressing zlib-compressed
records, and matching the `EDID` subrecord against `*sandbox*`) rather than trusting
any third-party FormID list — the actual game file is the ground truth. Chosen over
the `EditorLocation` variant (also found, `0x0009361E`) specifically because its
`PLDT` type reads "current location" rather than a fixed editor-placed marker: engaging
it on ANY generic NPC cannot send them walking off toward a marker that might be far
away or behind a locked door. Both are real, unmodified vanilla packages; radius 256.

**Engage:** NumpadEnter on an unclaimed aimed NPC offers the package; the pending
`EvaluatePackage(true,false)` runs on the next `Arbiter::OncePerFrame` and logs
`GetCurrentPackage` flipping to the client package AND the `ExtraAliasInstanceArray`
size (read under its own `BSReadWriteLock` via `BSReadLockGuard`, never mutated) — this
MUST be unchanged before/after; a change would mean the redirect somehow touched real
alias-fill state, which it must never do (CLAUDE.md #3).

**Release:** NumpadEnter again drops the claim and re-evaluates; expect the framework package
back with exactly one `OnPackageChange`.

**Save/load (Phase 3):** `kPreLoadGame` now calls `AliasPkgProbe::ClearOnPreLoad()` —
drops the claim WITHOUT any engine call (the actor is about to be replaced by the
incoming load, nothing to restore). The framework package resumes on the new save's
own first eval; no latch, no stale redirect surviving a load.

**Expected vs actual:**

| Check | Expected | Actual |
|---|---|---|
| Engage | `GetCurrentPackage` flips to `0x000956B8`; actor visibly sandboxes near its current spot | **PROVEN.** `GetCurrentPackage` flipped to the sandbox package (`0x000956B8`) on engage. Behavioral sandboxing itself is only visible on an OUT-OF-COMBAT NPC (a combat NPC's package is overridden by combat AI regardless of what `GetCurrentPackage` reports), so field-test the visible sandbox behavior on a calm NPC, not a claimed combatant. |
| `ExtraAliasInstanceArray` size | UNCHANGED before/after both engage and release | **PROVEN.** Size unchanged across engage and release — the redirect never touches real alias-fill state. |
| Release | framework package returns; exactly one `OnPackageChange` | **PROVEN.** Framework package restored cleanly on release. |
| Save/load mid-claim | framework package after load; no CTD; no stale redirect | Not exercised this run (Phase 3 not field-tested) — the `ClearOnPreLoad()` mechanism is in place but unconfirmed live. |

**Pass/fail: PROVEN for Phases 1-2 (engage/release).** Phase 3 (save/load) remains
unexercised. This confirms the 0x49 REDIRECT itself (not just the Phase-0 hook-fires
fact) is safe to build a real channel on later, for the engage/release path.

## Probe 3 — native-bit toggle (`kAttackingDisabled` / `kCastingDisabled`)

**Files:** `native/core/NativeBitProbe.{h,cpp}` — no hook, no RTTI, no VR gate (a
plain `Actor::BOOL_FLAGS` bit flip via `actor->GetActorRuntimeData().boolFlags`,
`stl::enumeration<BOOL_FLAGS,uint32_t>::set/reset`, is version-stable by construction).

Numpad1/Numpad2 re-aim the crosshair on every press (no sticky claim — a live Actor bit
needs no co-save handling for a throwaway toggle) and flip `kAttackingDisabled` /
`kCastingDisabled` respectively, logging the before/after state.

**Expected vs actual:**

| Check | Expected | Actual |
|---|---|---|
| `kAttackingDisabled` ON | the actor cleanly stops attacking (no stutter/wedge) | **Untested this run.** |
| `kAttackingDisabled` OFF | attacking resumes normally | **Untested this run.** |
| `kCastingDisabled` ON | the actor cleanly stops casting | **Untested this run.** |
| `kCastingDisabled` OFF | casting resumes normally | **Untested this run.** |

**Pass/fail: UNTESTED this field session** (the run focused on T1 observe/deny and the
0x49 redirect). PASSES per-bit if toggling ON cleanly stops the behaviour and toggling
OFF cleanly resumes it, with no wedge/stutter either direction — confirms the
"native-bit tier" of `ALLOWANCE-TEMPLATE.md` §3 as a usable wholesale-deny fallback.
Still to field-test.

## T4 — DEFERRED (built, field-crashed, removed 2026-09-03)

**T4 (`TESActionData::Process` body-command seat) is REMOVED.** It was built exactly
as designed — read the rel32 at valhallaCombat's known call site
(`RELOCATION_ID(48139,49170)+0x4D7`/`+0x435`), compared it against
`VTABLE_TESActionData[0]` slot 5, found a MISMATCH (devirtualised, as the design doc's
own caveat predicted — `CombatAnimation::Execute` calls `actionData.Process()` on a
by-value member), and installed the devirtualised fallback: an `SKSE::GetTrampoline()
.write_call<5>` 5-byte call patch at valhalla's own call site. **That patch crashed the
game.**

**Root cause (from the field crashlog):** the patched call site (module+0x7F9470) is
**also patched by SCAR.dll**, an installed attack-framework mod that hooks the exact
same AI attack-start path (`Hook_AttackStart.cpp` / `AIAttackStartHook::StartAttack`).
Crash stack: `[0] bad-execute 0x13FC79600 ← [1] T4Probe.cpp:104 ← [2] the patched site
module+0x7F9475 ← [3-8] SCAR`. Two independent 5-byte call patches at the same address,
installed by two different DLLs with no coordination, is a textbook stomp: whichever
patches second either overwrites the first's redirect or chains into a return address
SCAR's own trampoline no longer expects, landing execution at garbage. **This crashed
in ordinary combat, not during a probe keypress** — T4's call-site patch is live from
`Install()`, unconditionally, on every attack that routes through that site; there is
no hotkey gate on the hook itself (only on the OBSERVE-logging).

**Why "hook the callee entry instead" doesn't trivially fix this:** the callee entry
*is* the call target valhalla's site jumps to — SCAR's hook, per the crash stack,
appears to be at or wrapping the SAME site/entry (both mods targeting "where does an
AI attack actually start"), so patching the entry runs into the identical collision,
just moved one hop over. A safe T4 needs either (a) a genuinely different attach point
that observes/gates the same information WITHOUT patching a byte range another popular
mod is known to patch, or (b) real hook-chaining discipline — detect an existing patch
at the target site/entry (read-before-write, diff against the expected original bytes)
and CHAIN through it (call the current occupant, not assume you're the only patcher)
rather than blindly overwriting. Neither is a quick fix; this needs a proper design
pass before T4 is attempted again, not a hasty second call-site guess.

**This is not a dead end — coverage ADAPTS to T1, it doesn't just degrade
(`Docs/INVARIANTS.md` #17).** T1 (all 70 leaf `VTABLE_CombatBehaviorTreeNodeObject_*`
vtable hooks, `write_vfunc`) is a completely different, standard, chain-safe mechanism
— no call-site patch, no collision surface with SCAR or anything else — and it already
covers combat body-commands (attack/power-attack/block/bash and the rest of the leaf
catalog fire IN combat, which is where T4's crash happened too). Losing T4 does not
lose combat-action coverage: it falls through to T1, which was already armed and
unaffected. The real remaining gap is narrower than "all of T4" — it's the NON-combat
body-command slice `ALLOWANCE-TEMPLATE.md` §4 already called honest (sneak/draw/
activate/idle OUTSIDE combat, where no T1 leaf runs). Closing that slice later needs a
chain-safe seat for it specifically — a vtable attach point, or genuine detect-and-
chain discipline with whatever attack/action framework (SCAR or otherwise) already
patches the same site — never another raw call-site patch.

## A documentation note on SE/AE labels

While cross-checking `ALLOWANCE-TEMPLATE.md`'s cited VariantID numbers against the
real pinned headers (`alandtse/CommonLibSSE-NG@3f9fc67…`, the fork CPR itself builds
against, and `CharmedBaryon/CommonLibSSE-NG@c4ab853d…`, the colorglass-pinned commit
APMF actually builds against), every NUMBER matched exactly (Attack 213789/266747,
Block 212608/265987, CastImmediateSpell 213720/266705, `VTABLE_TESActionData`
188603/232777) — but in every case checked, the doc's inline "SE X / AE Y" prose had
the two labels swapped relative to the header's own positional convention
(`REL::VariantID`/`RelocationID`/`VariantOffset` all literally name their first
parameter `a_seID`/`a_seOffset` in `REL/Relocation.h`). This never leaks into any
hooked code here — every VariantID triple in `CombatBehaviorRE.h` is copied verbatim
(source order preserved), so the transposition is a narrative-label issue only. Worth
a follow-up pass over `ALLOWANCE-TEMPLATE.md`'s prose if it's edited again.
