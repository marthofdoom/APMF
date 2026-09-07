# SPEC — Porting MFO's Graduated Cast-Consent Slider onto APMF

> **STATUS BANNER (added 2026-09-07) — PARTIAL: the APMF half SHIPPED, the MFO half NEVER
> HAPPENED, and §3's finding was never measured.**
> - **APMF side: SHIPPED.** `APMF_SetSpellAllowList` exists (`core/ClientAPI.cpp`, ABI v4).
> - **MFO side: NOT DONE.** MFO has ZERO `SetSpellAllowList` / `SetCastAllowList` call sites
>   (grepped 2026-09-07), so the allow-list capability this spec was written for has never
>   been exercised by the reference client, and `Docs/INTEGRATION.md` still marks it unproven.
> - **The retirement this spec is a decision-input for never happened either.** MFO's own
>   `CheckCast` hook is still installed (thunk `native/CasterConsent.cpp:917`, `write_vfunc` at `:1054`), so BOTH 0x0A hooks
>   are live — MFO's sitting outside APMF's gate (`Docs/HOOK-SITE-COVERAGE.md` §6, the
>   two-repo overlap note; §5 is the procedure-tree gap, a different subject; MFO
>   `DIAG-2026-09-06-deny-heal-failures.md` row P12, LATENT).
> - **§3's finding — that castLvl 1-3 silently COLLAPSE under APMF — has never been
>   measured**, though it was written as "worth confirming on the next deck cycle" on
>   2026-09-03 and several deck cycles have run since. It is item 3 on `Docs/STATUS.md`'s
>   Next list.
> - `:10` cites MFO branch `feat/apmf-cast` @ `f80021f`, long since merged; read those
>   references as historical.

Research + design pass, 2026-09-03, read-only. Feeds marth's PORT-or-DROP call
on `MFO-CONVERSION-ROADMAP.md` facet #2 (`Docs/archive/MFO-CONVERSION-ROADMAP.md`,
archived 2026-09-07
row 2): if this port proves reliable, the graduated slider lives on APMF and
MFO retires its `CheckCast` hook; if not, DROP the slider and MFO becomes a
pure APMF cast client for the exact case only. Two repos: APMF
`/mnt/gaming/modlists/Projects/ai-package-management-framework`, MFO
`/mnt/gaming/modlists/Projects/marth-follower-overhaul` (branch
`feat/apmf-cast`, HEAD `f80021f`).

---

## 0. Scope precision (read this before the rest)

"Graduated cast consent" is actually **two separate mechanisms** bundled
under one MCM slider (`iCastControl` / `Config::g_castControl`), and they
need different treatment:

**(A) Gambit-spell DELIVERY** — how the player's *configured* cast-gambit
spell gets into the follower's hand and cast. Gated on `ClassifySpell` +
target, **not on castLvl at all**:
`Actuation.cpp:504-509`'s `ownedCast` guard —
```
APMFBridge::Available() && Config::g_apmfCast.load() && !Config::g_legacyCastHybrid.load() &&
a_target && a_target != a_follower && a_target != PlayerCharacter::GetSingleton() &&
ClassifySpell(spell) == SpellKind::Offense
```
has no `g_castControl` term. So for an **offense spell at a valid hostile
target**, delivery is *already* 100% owned-cast/APMF at every castLvl 1-4
today (facet #1, shipped). The AI-first-wait + `ForceCast` hybrid
(`Actuation.cpp:540-593`) only still runs delivery for: self-targeted casts
(actually see below — mostly NOT this path either), player-targeted casts,
and non-offense (buff/heal) gambits — i.e. it's gated on *spell kind and
target*, not on the slider.

Self-targeted casts and target-concentration casts don't even reach
`CheckStartCast`/`CheckCast` — they bypass consent entirely via
`CastSelfDirect`/`CastTargetDirect` (`Actuation_Direct.cpp:729,927`, direct
`CastSpellImmediate`, confirmed `Actuation.cpp:211-220`). That is a
**deliberately forced** delivery mechanism with no AI deliberation at all —
there is no "allow-list" concept to port there; it stays native regardless
of this spec's outcome and is **out of scope**.

So mechanism (A)'s only real gap is: **non-concentration heal/buff gambits,
and any gambit targeted at the player**, which still always use the
AI-first-wait+ForceCast legacy hybrid, at every castLvl, APMF present or not.
Widening `ownedCast`'s guard to admit these is a separate design call from
the ABI work below (§5).

**(B) The exemption gate for the follower's OWN, non-gambit spell attempts**
— "let the AI keep casting buffs/heals on its own initiative while under
cast control." This is `CastExempt` (`CasterConsent.cpp:112-118`) consulted
by `CtrlUnlatchedDeny` (`:206-229`, un-latched/continuous) and by
`ShouldDeny`/`CheckCastThunk`'s `exclusivityDeny` (`:681-698`, `:841-846`,
the latched case). **This is castLvl's real target**, and — this is the
useful discovery of this pass — it is **not** an AI-first-wait hybrid at
all. It is a pure ALLOW/DENY gate on an arbitrary candidate spell, exactly
the shape of APMF's T2c `CheckCast` template already. The only thing it does
that APMF's *current* claim can't is allow **more than one form**.

This spec is therefore mostly about (B): give APMF's `SelectSpell` claim a
multi-form allow-set, so `CastExempt`'s policy can be expressed as data
instead of native code. (A)'s remaining native slice (self/player-targeted,
non-offense gambits) is flagged in §5 as a second, smaller, separable
widening — do it after (B) if this proves out, not necessarily in the same
change.

---

## 1. Current MFO mechanism (facts, with citations)

- `ClassifySpell` (`CasterConsent.cpp:24-36`) — MFO's classifier, walking a
  `MagicItem`'s own effects: any hostile/detrimental effect → `Offense`
  (whatever else it does); else a beneficial `kHealth` effect → `Heal`; else
  `Buff`. This is **effect-based**, not a fixed compile-time category — it
  can classify *any* spell any follower happens to know, including ones MFO
  has never seen before (perk-granted, mod-added).
- `CastExempt(kind, selfHeal, lvl)` (`:112-118`) — the 4-level slider:
  lvl 1 keeps buffs+heals to the AI, lvl 2 keeps all heals, lvl 3 keeps
  self-heals only, lvl≥4 (exact) exempts nothing.
- `CtrlUnlatchedDeny` (`:206-229`) — the continuous, un-latched half: for a
  tracked follower with configured cast gambits (`g_ctrl`, populated by
  `NoteGambits`, `CasterConsent.h:77`), a gambit spell always passes; else
  exact denies everything, partial applies `CastExempt`.
- `ShouldDeny`/`CheckCastThunk`'s `exclusivityDeny` (`:681-698`, `:841-846`)
  — the latched, hard pre-charge mirror of the same policy, at the `CheckCast`
  (0x0A) gate — the one that actually stops a charge (`CheckStartCast` alone
  is advisory and leaks, per the file's own v1.0.36 note, `:653-667`).
- **The existing stand-down**: `CheckCastThunk` still computes
  `exclusivityDeny` via `ShouldDeny` (which already applies `CastExempt`) but
  does not *act* on a `true` verdict when
  `APMFBridge::IsOwnedCastActive(fid)` (`:843`) — i.e. when APMF already
  holds this follower's `SelectSpell` claim (mechanism A above, live for the
  offense gambit). The intent (`:835-840`'s comment) is "APMF's own CheckCast
  hook already enforces this exact exclusivity ... running MFO's own deny
  here too would be redundant." That's true for the *gambit-spell-vs-anything-else*
  binary case — but see §3 for why it is **not actually equivalent** once
  `CastExempt` would have allowed the candidate.

## 2. Current APMF mechanism (facts, with citations)

- `kIntent_SelectSpell` (ch.8) claim: arbitration only, no engine write
  (`channels/CastingSelect.cpp:11-28`). Enforcement is T2c, `CastGate.cpp`,
  hooking `MagicCaster::CheckCast` (0x0A) on `VTABLE_ActorMagicCaster[0]`
  (`CastGate.cpp:92-98`) — the same slot MFO's own `CheckCast` hook patches
  (`CasterConsent.cpp:897-899`), currently a documented, unresolved
  double-hook (`HOOK-SITE-COVERAGE.md` §6, `Docs/archive/MFO-CONVERSION-ROADMAP.md`
  row 1).
- `CastGate::CheckCastThunk` (`CastGate.cpp:47-76`): lets the engine answer
  first, resolves the actor via `GetCasterAsActor()` (`:66`), then
  `allowance::Allowed(fid, kIntent_SelectSpell, subjectForm)` (`:71`).
- `Allowance::Allowed` (`Allowance.cpp:60-65`) — **the binary gate**:
  ```cpp
  APMF_API::APMF_Param claim{};
  if (!ControlMap::Get().TryGetOwningClaim(actor, intent, claim)) return true;
  if (claim.form == 0) return true;
  return claim.form == subjectForm;
  ```
  Exact match only. No concept of "or one of these others."
- `ControlMap::Claim` (`ControlMap.h:123-127`) carries exactly one
  `APMF_Param param` per claim; `TryGetOwningClaim`
  (`ControlMap.cpp:347-382`) returns the single **winning** claim's param
  (highest `basis`, tie → earliest) — arbitration is **winner-take-all per
  channel**, not a union of every claim on that channel.
- `APMF_Param` (`APMF_API.h:158-163`): `{ RE::FormID form; float fval;
  std::int32_t ival; }` — fixed POD, no array, shared by every intent.
- Precedent for a bitmask param already exists: `kIntent_CombatAction` (ch.7)
  carries a `CombatActionCategory` bitmask in `param.ival`
  (`APMF_API.h:121,129-145`), checked by `ActionGate.cpp:127-129`
  (`(denyMask & leafCat) == 0` → allow). **But** the categories it bitmasks
  are ~70 **fixed, compile-time-known** behavior-tree leaves, classified
  *once at install* against a static name list (`ActionGate.cpp:61-74`) —
  not a runtime-varying, per-follower, arbitrary `SpellItem` FormID set. This
  distinction is exactly why option (b) below fails for spells (§4.2).
- `APMF_API.h`'s versioning: COM-style prefix extension, `APMF_API_v1..v3`
  (`:168-229`), `kABIVersion = 3` (`:80`). Append-only: new function slots
  only at the end of a new `APMF_API_vN`, `abiVersion` field gates what a
  client may read. `EnsureClaimLocked`/`EnsureIvalClaimLocked`
  (`MFO/native/APMFBridge.cpp:79-128`) already show MFO's own client-side
  pattern for `RequestEx`+`Repoint` (v3) with a v2 release+request fallback —
  reusable template for the new call.

## 3. The load-bearing finding: the graduated exemption is *already* at risk today

This is inference from reading the chaining logic, **not field-confirmed** —
flag for verification, but it follows directly from the code as written and
materially affects the reliability read.

Both MFO's native `CheckCast` hook and APMF's T2c `CheckCast` hook are
*currently* installed on the same vtable slot (`HOOK-SITE-COVERAGE.md:
294-299`). `write_vfunc` means whichever plugin installs **second** becomes
the outer thunk, chaining to the first as its "original." Trace both orders
for the case that matters — a follower has a live `SelectSpell` claim on
spell X (mechanism A, active for *any* configured offense gambit at *any*
castLvl, see §0) and its AI now tries an exempt candidate spell Y (a heal,
castLvl 2):

- **MFO outer, APMF inner:** MFO's thunk calls `orig()` = APMF's thunk first.
  APMF's thunk computes `engineSays` from the *true* engine, then applies its
  own binary `Allowed()` — `X != Y` → **deny**, returned as `aiOK=false` to
  MFO's outer thunk, which immediately returns false at `CheckCastThunk:793`
  (`if (!aiOK) return false;`) **before MFO's own `CastExempt`-aware logic
  ever runs**.
- **APMF outer, MFO inner:** APMF's thunk calls `orig()` = MFO's thunk first,
  which correctly computes `exclusivityDeny=false` for the exempt heal and
  returns `true`. APMF's *own* logic then still runs unconditionally on that
  `true`: `Allowed()` sees claim.form=X, subjectForm=Y, still **denies** —
  MFO's semantically-correct exemption is silently overridden by APMF's
  binary check regardless of what MFO decided underneath it.

**Both install orders deny the exempt spell.** If this reading is right, the
moment a follower has *any* configured offense gambit and APMF present
(today's default, per facet #1), castLvl 1-3 is already effectively
neutralized down to castLvl-4 behavior for that follower for the duration of
the claim — the double-hook doesn't just duplicate enforcement, it silently
breaks the graduation the slider promises. This is independent of whether
marth ports or drops facet #2: **closing out facet #1's hook cleanup
(`Docs/archive/MFO-CONVERSION-ROADMAP.md` §3 item 0) should confirm/measure this on the
next deck cycle regardless**, and it strengthens the case that a real
multi-form allow-list (this spec) is a *correctness fix*, not just a nice
port.

## 4. ABI design for a multi-form allow-list

### 4.1 Requirements
- POD, C-ABI, append-only — `APMF_API.h`'s frozen contract (`:13-19`).
- MFO owns spell classification (`ClassifySpell`); APMF must do **pure
  membership testing**, no domain knowledge of spell effects.
- Bounded, no heap traffic across the boundary or in the hot `CheckCast`
  thunk (combat thread, `ALLOWANCE-TEMPLATE.md:115` confirms non-main).
- Must not perturb `APMF_Param`'s existing use by every other intent (ch.6
  combat-target, ch.9 offer-package, ch.15 equipment, ch.7's bitmask, ...).

### 4.2 Options evaluated

**(a) A new v4 function taking a bounded array — RECOMMENDED.**
```cpp
// Appended to a NEW APMF_API_v4 (v1-v3 byte-identical, unchanged):
void (*SetSpellAllowList)(Handle handle, const RE::FormID* forms, std::uint32_t count);
```
Attaches a bounded "also-allowed" set to an *existing* claim (created via the
normal `RequestEx(actor, kIntent_SelectSpell, basis, &primaryParam)` exactly
as today — `param.form` stays the primary/gambit spell). `forms` is read and
copied synchronously inside the call, same contract as `RequestEx`/`Repoint`
(`APMF_API.h:61-62`, mirrored by `Allowance`'s "never invent a YES" rule).
`count` is capped (`kMaxSpellAllowList = 32`, chosen generously above what a
follower's buff+heal spell list realistically holds — see §5's sizing note)
and silently clamped with a one-time log on overflow, never a hard error
across the DLL boundary (matches the "no exception ever crosses" rule,
`APMF_API.h:64-68`). `count == 0` / `forms == nullptr` clears the allow-set —
falls back to today's exact binary match, so **a v1-v3 client, or a v4
client that never calls this, is byte-for-byte unaffected.**

**(b) A category bitmask in `param.ival`, mirroring ch.7 — REJECTED.**
Ch.7's bitmask works because the ~70 leaves it classifies are a **fixed,
compile-time set** — classified once at install against a hardcoded name
list (`ActionGate.cpp:36-74`). Spells are the opposite: an open,
runtime-varying, per-follower set of arbitrary `SpellItem` FormIDs (custom
mod spells included) that only `ClassifySpell`'s effect-walk
(`CasterConsent.cpp:24-36`) can classify. For APMF's `CheckCast` thunk to
honor a bitmask, **APMF itself would have to classify the candidate spell**
at deny time — duplicating `ClassifySpell`'s logic inside APMF.dll. That
means: (1) MFO no longer owns classification (violates the stated design and
risks drift between two independent implementations of "is this hostile"),
and (2) APMF would need to walk `MagicItem::effects` on the combat thread for
every candidate cast, every follower, forever — real per-tick cost the
current template deliberately avoids (`Allowance.h`'s template comment,
`:24-29`, treats `Allowed` as "the one shared flip YES→NO decision," not a
place for domain logic). Rejected — this is the exact tension the brief
asked to be assessed, and it fails.

**(c) Multiple claims on the same channel — REJECTED.**
`ControlMap::TryGetOwningClaim` (`ControlMap.cpp:347-382`) is winner-take-all
per channel per actor (highest `basis`, `:372-377`): it returns **one**
claim's param, never a union of every claim on that channel. Multiple
`SelectSpell` claims from MFO on the same follower would not compose into
"allow any of these forms" — the highest-basis one simply wins outright and
the rest sit inert. Making claims union instead of arbitrate would be a
core-semantics change affecting *every* intent (ch.6, ch.8, ch.9, ch.15, ...
all rely on single-owner-per-channel), not a scoped addition — rejected as
disproportionate to the problem.

### 4.3 The chosen shape, in full

- `APMF_API.h`: append `APMF_API_v4` (v1-v3 members verbatim, then
  `SetSpellAllowList`), bump `kABIVersion = 4`. Add a
  `kMaxSpellAllowList = 32` constant next to the array param.
- `ControlMap.h`'s private `Claim` struct (`:123-127`) gains a fixed member:
  `std::array<RE::FormID, kMaxSpellAllowList> altForms{}; std::uint8_t altCount = 0;`
  — bounded, no allocation, cheap enough for the RCU snapshot's per-Publish
  deep-copy (`NpcCtl`'s copy ctor, `ControlMap.h:148-150`; the claims vector
  is already deep-copied per generation, this just adds ~132 bytes per claim
  that has one — the controlled set is small, INVARIANTS #13).
- New `PendingOp::Kind::kSetAllowList` (`ControlMap.h:160`), carrying
  `handle` + the array + count, enqueued through the existing `m_qmx`-guarded
  path exactly like `Repoint` (`ControlMap.h:72`, `:159-166`).
- `ApplySetAllowList(handle, forms, count, map)` (writer-thread only,
  alongside `ApplyRepoint`): looks the claim up via `m_index` exactly as
  `ApplyRepoint` does, writes `altForms`/`altCount` on the matching `Claim`.
- `ControlMap::TryGetOwningClaim` — **this is APMF-internal C++, not part of
  the C-ABI**, so it's free to change shape. Add an optional out-parameter:
  ```cpp
  bool TryGetOwningClaim(RE::FormID actor, Intent intent, APMF_API::APMF_Param& outParam,
                         std::span<const RE::FormID>* outAllowSet = nullptr) const;
  ```
  `outAllowSet`, when non-null, is pointed at the winning claim's `altForms`
  data (`{altForms.data(), altCount}`) — safe because the reader's local
  `shared_ptr<const MapType>` snapshot copy (`ControlMap.cpp:358`) keeps that
  generation alive for the whole call, same RCU discipline as today.
- `Allowance::Allowed` (`Allowance.cpp:60-65`) extends to:
  ```cpp
  bool Allowed(RE::FormID actor, APMF_API::Intent intent, RE::FormID subjectForm) {
      APMF_API::APMF_Param claim{};
      std::span<const RE::FormID> allowSet;
      if (!ControlMap::Get().TryGetOwningClaim(actor, intent, claim, &allowSet)) return true;
      if (claim.form == 0 && allowSet.empty()) return true;
      if (claim.form == subjectForm) return true;
      return std::ranges::find(allowSet, subjectForm) != allowSet.end();
  }
  ```
  `CastGate.cpp:71` is unchanged (still calls `allowance::Allowed(fid,
  kIntent_SelectSpell, subjectForm)`) — the widening is entirely inside
  `Allowed`/`ControlMap`, invisible to the thunk. `EquipGate.cpp`'s T2a
  (`Allowed(fid, kIntent_SelectSpell, ...)`, per the roadmap's facet #6 note)
  gets the same widening for free, which is *also* correct: a follower whose
  AI wants to re-equip an exempt heal spell should not be blocked from
  arming it either.
- **Append-only guarantee**: v1-v3 struct layouts, `APMF_Param`, every
  existing `Intent`/`CombatActionCategory` value, and every other channel's
  behavior are untouched. A claim that never calls `SetSpellAllowList` has
  `altCount == 0`, and `Allowed()`'s new branch is unreachable — byte-for-byte
  today's binary behavior. This is the same append-only discipline
  `APMF_API.h:13-19` already mandates and `Repoint`'s v3 addition already
  demonstrates the pattern for (`APMF_API.h:207-229`).

## 5. MFO's side: where the allow-set is computed and pushed

- **Compute site**: the natural home is alongside `NoteGambits`
  (`CasterConsent.h:77`, populates the persistent `g_ctrl` map,
  `CasterConsent.cpp:92`, called every combat-service tick per the file's own
  comment `:79-81`). Extend that same tick to also compute, per tracked
  follower currently under cast control: `allowSet = g_ctrl[fid] ∪
  {every currently-known spell of a CastExempt(kind, selfHeal, castLvl)==true
  kind}`. Enumerating "every currently-known spell" needs an actor
  known-spell walk MFO does not have today (`ClassifySpell` only classifies
  a spell handed to it, it never enumerates); **this is new work, and the
  exact CommonLib accessor (candidates: `ActorBase`'s added-spell list vs. a
  runtime spell-list walk) needs confirming at implementation time** — flagged
  uncertainty, not verified in this read-only pass.
- **Push site**: `APMFBridge.h`/`.cpp` gets a new `SetCastAllowList(FormID
  follower, std::span<const RE::FormID> forms)` mirroring
  `ClaimCasting`/`EnsureClaimLocked`'s existing shape
  (`APMFBridge.cpp:196-204`, `:79-101`) — gated on `abiVersion >= 4` exactly
  as `Repoint` is gated on `>= 3` (`APMFBridge.cpp:93,120`). Called every
  tick `ClaimCasting` is called (`Actuation.cpp:522`), so it rides the same
  refresh/expiry lifecycle (`kExpiry`, `APMFBridge.cpp:73`) — no new
  lifecycle to reason about.
- **Sizing**: `kMaxSpellAllowList = 32` (§4.3) needs to comfortably cover a
  follower's realistic known heal/buff spell count (typically single digits
  to low teens even for a heavily-modded mage build) — safe headroom, and an
  overflow degrades to "the excess spells are treated as non-exempt" (still
  denied, never a crash or an unbounded write), which is a *safe* direction
  to fail in.
- **Stand-down**: mirror `IsOwnedCastActive` (`APMFBridge.cpp:186-193`).
  Once the allow-set is being pushed and the ABI is ≥4, `CtrlUnlatchedDeny`
  (`CasterConsent.cpp:206-229`) and `ShouldDeny`/`CheckCastThunk`'s
  `exclusivityDeny` (`:681-698`, `:841-846`) should stand down their own
  `CastExempt` evaluation for a follower under an active APMF allow-set claim
  — the same pattern `IsOwnedCastActive` already uses at `:843`, just gating
  on a new `APMFBridge::IsAllowListActive(fid)` rather than
  `IsOwnedCastActive`. Doing this **removes the double-hook problem for
  facet #2 specifically** (§3) — once APMF's `Allowed()` itself understands
  the exempt set, there is no more competing native deny to race against, in
  either install order.
- **(A)'s separate widening** (§0): if marth also wants self/player-targeted
  and non-offense gambits ported, that's a second, smaller change to
  `ownedCast`'s guard (`Actuation.cpp:504-509`) dropping the
  `ClassifySpell==Offense` and self/player exclusions — orthogonal to the ABI
  work above, and lower-risk to sequence *after* §4's mechanism is field-proven
  since it touches the currently-working self-cast/legacy paths.

## 6. Semantic-fidelity / reliability verdict

The brief's framed tension — "AI-first-wait HYBRID vs. hard allow-list gate,
do they feel the same" — turns out to be **the wrong comparison for the part
that actually needs porting**. As shown in §0, mechanism (B) (the exemption
gate castLvl 1-3 actually governs) was **never** an AI-first-wait+force
hybrid — it is *already* a pure hard ALLOW/DENY gate on the AI's own
initiative (`CtrlUnlatchedDeny`/`ShouldDeny` never force anything; they only
ever return true/false). A hard gate is not an approximation of a hard gate —
it's the same mechanism, just currently expressed as MFO-native `unordered_map`
+ `CastExempt` code instead of an APMF claim + allow-set. **This is a faithful
port, not a behavior change**, *provided* the allow-set is computed correctly
(the right spells, kept live/refreshed exactly when `g_ctrl`/`CastExempt`
would have exempted them) — which §4-5 give a concrete, POD, append-only path
to.

Where fidelity genuinely needs care:
- **Freshness.** MFO's native gate evaluates `CastExempt` **live, per
  candidate spell, every deliberation** (`CtrlUnlatchedDeny` reads
  `Config::g_castControl.load()` fresh each call, `:209`) — an MCM slider
  change takes effect mid-combat, per the file's own comment (`:201-202`,
  "read off the LIVE slider so an MCM change applies mid-combat"). The
  APMF allow-set is instead **pushed periodically** (per combat-service
  tick, §5) and read from a **snapshot published once per Drain**
  (`ControlMap.h`'s RCU model). A slider change or a newly-learned spell
  won't take effect until the next push+Drain cycle — bounded by the pump
  cadence (`kPumpMs=133`, `CLAUDE.md`), not instant. This is a real but
  small latency difference (~1-2 pumps), not a behavior change.
- **The claim/allow-set must be kept alive whenever ANY exemption should
  apply**, not just during an active offense-gambit's delivery window. Today
  `CtrlUnlatchedDeny` runs for *every* tracked, cast-gambit-configured
  follower in combat, latched or not (`:92`'s comment: "continuous in
  combat, latched or not"). If the port only pushes the allow-set while
  mechanism (A)'s `ClaimCasting` is separately active (i.e. only during an
  offense-gambit's own cast), there is a **gap** identical in shape to the
  bug this port is meant to fix (§3) for the periods where the follower is
  under cast control but *not* mid-gambit-cast. §5's "compute alongside
  `NoteGambits`, push on its own cadence" design avoids this by decoupling
  the allow-set claim's lifecycle from mechanism (A)'s claim lifecycle — but
  this decoupling is the single most important implementation detail to get
  right, and worth its own explicit field-test check (§8).
- **What does NOT need porting and should not be conflated**: the
  concentration hard-bound (`ConcUnboundedDeny`, `:152-175`, `:826-829`) is
  explicitly latch-independent, exact-only, and — per the file's own
  header comment — a safety rule that "runs unconditionally, never stands
  down for APMF." It is out of scope; nothing in this spec touches it.

**Verdict: PORT.** The mechanism being ported (B) is already the same shape
APMF's template wants (a hard allow/deny gate on an arbitrary subject form);
the ABI gap is a real but bounded, append-only, POD-safe addition with a
direct precedent (ch.7's bitmask taught the "don't push classification into
APMF" lesson, which this design respects by keeping classification in MFO
and shipping APMF only a resolved FormID list); and — independent of
"port or drop" — §3's finding means the *current* unported state may already
be silently broken for any offense-gambit follower with APMF present, which
argues for fixing this promptly rather than leaving castLvl 1-3 as a
known-buggy no-op. DROP would mean accepting that castLvl 1-3 quietly
degrades to castLvl 4 behavior whenever APMF is present and an offense gambit
is configured — not neutral, a regression from what the slider promises.

## 7. Risk / effort estimate

**APMF repo:**
- `APMF_API.h`: append `APMF_API_v4` + `SetSpellAllowList` + `kMaxSpellAllowList`
  — mechanical, low risk (pure append, same pattern as v2→v3).
- `ControlMap.h`/`.cpp`: new `Claim` fields, new `PendingOp::Kind`, new
  `ApplySetAllowList`, extended `TryGetOwningClaim` overload — moderate size,
  low-to-moderate risk. The RCU/snapshot discipline (INVARIANTS #12/#13) must
  be respected exactly as the existing `Repoint` path does; this is a
  well-trodden extension of an existing pattern, not new architecture.
- `Allowance.cpp`: `Allowed()` extension — small, low risk, but is the
  **hot combat-thread path** (T2c/T2a both call it) — the added
  `std::ranges::find` over ≤32 elements is cheap but should be benchmarked
  once alongside the existing "near-zero for an uncontrolled NPC" pre-gates.
- Net: **small-to-moderate** — a well-scoped, append-only extension of
  machinery that already exists (`Repoint`'s v3 precedent + ch.7's bitmask
  precedent cover most of the hard design questions already).

**MFO repo:**
- New known-spell enumeration (§5) — **the one genuinely new piece of work**,
  unverified API surface, needs a short research spike before implementation.
- `APMFBridge`: `SetCastAllowList` + `IsAllowListActive` — small, mirrors
  existing `ClaimCasting`/`IsOwnedCastActive` almost exactly.
- `CasterConsent.cpp`: stand-down wiring in `CtrlUnlatchedDeny`/
  `ShouldDeny`/`CheckCastThunk` — small, same shape as the existing
  `IsOwnedCastActive` stand-down at `:572,843`.
- `Actuation.cpp`: the compute-and-push call site alongside `NoteGambits` —
  small.
- Net: **small**, gated on the spell-enumeration research spike landing
  cleanly.

**Combined risk the roadmap already flags**: this work should be sequenced
*after* facet #1's cleanup (removing MFO's now-redundant native `CheckCast`
hook once APMF's T2c has its own deck pass, `Docs/archive/MFO-CONVERSION-ROADMAP.md` §3
item 0) — doing the allow-set work while two independent `CheckCast` hooks
still fight on the same vtable slot makes the field test in §8 hard to read
cleanly (a failure could be either problem).

## 8. What a field test should check

1. **Baseline regression**: with the allow-list ABI live but `SetCastAllowList`
   never called by MFO (or ABI<4), confirm the game behaves byte-identical to
   today — a v1-v3-shaped smoke test.
2. **Exemption correctness at each castLvl**: a follower with a configured
   offense gambit, at castLvl 1/2/3, should be observed casting a self-heal
   / heal-other / buff on its own AI initiative *while the gambit claim is
   live* — this is the exact scenario §3 argues is broken today. Confirm the
   deck log shows the AI's own cast succeeding, not an `AbortLog`/`DenyLog`
   line.
3. **The decoupling gap** (§6): a follower under continuous cast control but
   *between* gambit casts (no live `SelectSpell` claim for the gambit spell
   itself right now) should still exempt the configured kinds — confirm the
   allow-set claim's own lifecycle (independent of mechanism A's) is actually
   alive during that gap, not just during an active cast.
4. **Freshness**: flip the MCM slider mid-combat; confirm the AI's exemption
   set changes within a pump or two, not "next combat."
5. **Overflow behavior**: a follower with more known exempt-kind spells than
   `kMaxSpellAllowList` — confirm graceful truncation (some spells denied,
   never a crash, never an unbounded write) and that the truncation is logged
   once, not spammed.
6. **Double-hook interaction**: run this BEFORE and AFTER facet #1's hook
   cleanup lands, to separate "the allow-set design works" from "the
   double-hook install-order was masking/compounding it" — §3's finding means
   these two are currently entangled and should be measured separately.

---

## Summary (for the calling agent)

- **Recommended ABI**: append `APMF_API_v4` with one new function,
  `SetSpellAllowList(Handle, const RE::FormID* forms, std::uint32_t count)`
  (bounded, `kMaxSpellAllowList=32`, copied synchronously) — attaches a
  bounded allow-set to an existing `SelectSpell` claim. `ControlMap::Claim`
  gains fixed `altForms`/`altCount`; `Allowance::Allowed`
  (`Allowance.cpp:60-65`) checks `claim.form == subject || subject ∈
  altForms`. v1-v3 clients and every other intent are byte-identical.
  Rejected: a category bitmask (would push spell classification into APMF,
  breaking MFO's ownership of `ClassifySpell` and adding per-tick effect-walk
  cost) and multiple claims (ControlMap arbitration is winner-take-all per
  channel, not a union — would need a core-semantics change).
- **PORT, not drop.** The mechanism castLvl 1-3 actually governs
  (`CtrlUnlatchedDeny`/`ShouldDeny`) is *already* a pure hard allow/deny
  gate, not an AI-first-wait hybrid — the same shape APMF's T2c template
  wants. The ABI gap is real but bounded and append-only-safe.
- **Live finding, not yet field-confirmed**: reading the current double-hook
  chaining (both MFO's and APMF's `CheckCast` on the same vtable slot,
  `HOOK-SITE-COVERAGE.md:294-299`) suggests castLvl 1-3 may *already* be
  silently collapsing to castLvl-4 behavior for any follower with a
  configured offense gambit and APMF present — independent of this port,
  worth confirming on the next deck cycle.
- Full detail, citations, ABI struct layout, MFO wiring, and a 6-point field-
  test checklist are in `Docs/SPEC-GRADUATED-CAST.md`.
