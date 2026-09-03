# Hook-site coverage: CommonLibSSE-NG bindings + Address Library IDs

Status: **static research spike (2026-09-03), READ-ONLY, no code changes.** Enumerates every
vtable hook / call-site trampoline installed by APMF and MFO today, and checks each one against
the pinned CommonLibSSE-NG commit for two SEPARATE things that go to two SEPARATE upstream
projects:

1. **CommonLib binding** — does `CharmedBaryon/CommonLibSSE-NG` expose a named, typed C++ class/
   vtable-slot for this site, or is it `Unk_XX` / entirely absent? Gap here → a CommonLibSSE-NG PR.
2. **Address Library ID** — does our code resolve the site's address via a `REL::VariantID`/
   `REL::RelocationID`/`RE::VTABLE_*` triple (i.e. an existing SE+AE Address Library id), or via a
   raw offset / signature scan? Gap here → a submission to the Address Library id database
   (a different project with a different process — versionlib, not CommonLib).

**Scope, per marth's 2026-09-03 sharpener: this file requests bindings ONLY for sites we
actually hook today (or concretely need next, e.g. the non-alias package slot under
investigation) that are UNDOCUMENTED (`Unk_XX` or absent) in CommonLib.** Every site whose
CommonLib slot is already named+indexed is marked **"covered, no PR"** below and gets nothing —
not because it's unimportant, but because there's nothing to ask upstream for. Overlay/trampoline
call-site patches are out of scope for CommonLib on principle (see §4) regardless of naming
status.

**Bottom line up front:** of 10 hooked sites across both repos, **9 are already fully covered**
(named CommonLib slot + existing Address Library id). **Exactly one CommonLib gap is PR-ready**
(the `CombatBehaviorTreeNode` class family — T1/ch.7's ~70-leaf combat-action gate), and it needs
**zero new Address Library ids** (every id it needs already exists in the pinned tree; the gap is
purely a missing C++ class binding over addresses that are already there). One further site
(`BGSProcedureTreeProcedure`'s `Unk_XX` slots) is flagged as a real gap but is **not ready** — no
PR, pending an RE spike. See §5.

## Method / sources

- Pinned CommonLib commit: **`c4ab853d095e81e3390b282d7ba01ab2f24ebf25`**,
  `CharmedBaryon/CommonLibSSE-NG` (cited by `native/core/CombatBehaviorRE.h`'s header comment;
  `native/vcpkg-configuration.json` pins the port via the `colorglass/vcpkg-colorglass` registry).
  All header claims below were fetched live from
  `https://raw.githubusercontent.com/CharmedBaryon/CommonLibSSE-NG/c4ab853d.../<path>` at that
  exact SHA (not `main`), cross-checked against a recursive GitHub API tree listing at the same
  SHA for file-existence claims.
- This spike reuses and re-verifies (rather than re-derives from scratch) the header citations
  already collected by `Docs/ALLOWANCE-TEMPLATE.md` and `Docs/PROBE-NONALIAS-PACKAGE.md` — both
  read before this pass, both independently re-checked against the live headers below.
- Both local repos (`ai-package-management-framework`, `marth-follower-overhaul`) were grepped
  for `write_vfunc`, `write_call`, `write_branch`, `write_thunk`, `InstallOnVtables`,
  `RELOCATION_ID`, and `REL::Relocation` to enumerate every hook/trampoline site; nothing else
  matched.

---

## 1. Site inventory + coverage table

| # | Repo | Site (file:line) | Class + slot / call-site | What it hooks | Hook style | CommonLib status | Address Library status | PR-worthy? |
|---|---|---|---|---|---|---|---|---|
| 1 | APMF | `native/core/Hook.cpp:94-98` | `RE::VTABLE_Character[0]` / `RE::VTABLE_PlayerCharacter[0]`, slot **0xAD** | `Actor::Update(float)` | chainable vtable hook | **NAMED** — `Actor.h:367` `SKYRIM_REL_VR_VIRTUAL void Update(float a_delta); // 0AD` | ID-backed (`RE::VTABLE_Character`/`VTABLE_PlayerCharacter` embed SE+AE ids) | N — covered, no PR |
| 2 | APMF | `native/core/CastGate.cpp:92-98` | `RE::VTABLE_ActorMagicCaster[0]`, slot **0x0A** | `MagicCaster::CheckCast` (hard pre-charge cast gate) | chainable vtable hook | **NAMED** — `MagicCaster.h:55` `virtual bool CheckCast(MagicItem*, bool, float*, MagicSystem::CannotCastReason*, bool); // 0A` | ID-backed | N — covered, no PR |
| 3 | APMF | `native/core/EquipGate.cpp:107-141,144` | 30× `VTABLE_CombatInventoryItemMagicT_...[0]`, slot **0x0F** | `CombatInventoryItem::CheckShouldEquip` | chainable vtable hook, 30 concrete instantiations | **NAMED** — `CombatInventoryItem.h:73-74` `virtual bool CheckBusy(...); // 0E` / `virtual bool CheckShouldEquip(...); // 0F - { return !a_controller->state->isFleeing; }` | ID-backed (all 30 symbols exist with SE+AE ids) | N — covered, no PR |
| 4 | APMF | `native/core/ActionGate.cpp:152-156`, `native/core/CombatBehaviorRE.h:105-176` | 70× `VTABLE_CombatBehaviorTreeNodeObject_*[0]` (locally re-declared triples, verified identical to the same symbols already in the pinned tree's own `Offsets_VTABLE.h`), slot **0x02** | `CombatBehaviorTreeNode::act()` ("Enter") — the T1/ch.7 combat-action allowance, all ~70 combat behavior-tree leaves | chainable vtable hook, 70 concrete leaves | **ABSENT** — no `CombatBehaviorTreeNode.h`/`CombatBehaviorThread.h`/`CombatBehaviorTreeControl.h`/`CombatBehaviorController.h` anywhere in the pinned tree (confirmed via a full recursive GitHub tree listing: zero paths match `*CombatBehavior*.h`) even though the raw vtable/RTTI **addresses** already exist (see §2) | **Addresses already ID-backed** — `RE::VTABLE_CombatBehaviorTreeNode` (`Offsets_VTABLE.h:3426`, ids `265775`/`212199`), `RE::VTABLE_CombatBehaviorTreeNodeObject_CombatBehaviorAttack_` etc. (all 70, e.g. `Offsets_VTABLE.h:3723`, ids `266747`/`213789`), `RE::RTTI_CombatBehaviorTreeNode` (`Offsets_RTTI.h:1994`, ids `686393`/`394204`) — every id this site needs is already in the pinned tree | **Y — CommonLib class binding, §3** |
| 5 | APMF | `native/core/PackageGate.cpp:70-71` | `RE::VTABLE_Character[0]`, slot **0x49** | `Actor::CheckForCurrentAliasPackage` (ch.9 package-offer allowance) | chainable vtable hook | **NAMED** — confirmed by `Docs/PROBE-NONALIAS-PACKAGE.md` §2 direct citation of `TESObjectREFR.h:49`/`Actor.h:049`: `[[nodiscard]] TESPackage* CheckForCurrentAliasPackage() override; // 049` | ID-backed | N — covered, no PR |
| 6 | APMF | `native/core/PackageGate.cpp:78-83` | `Actor::EvaluatePackage(bool,bool)` call (nudge, not a hook) | not a vtable/RTTI site — a plain non-virtual member call, resolved locally by `RELOCATION_ID(36407,37401)` | non-virtual function call via local relocation | **Already fully bound in CommonLib** — `Actor.h:519` declares it and `Actor.cpp:249-` defines it against the SAME ids (`RELOCATION_ID(36407,37401)`) | ID-backed (already CommonLib's own) | N — not a gap; **code-hygiene note**: this could call `a_actor->EvaluatePackage(true,false)` directly instead of re-declaring the relocation locally (§5) |
| 7 | MFO | `native/Targeting.cpp:123-159` | `RE::VTABLE_Character[0]`, slot **0xE4** | `Character::UpdateCombat` (target-redirect + weapon-stance ownership) | chainable vtable hook | **NAMED** — `Actor.h:422` `SKYRIM_REL_VR_VIRTUAL void UpdateCombat(); // 0E4` | ID-backed | N — covered, no PR |
| 8 | MFO | `native/CasterConsent.cpp:670,898,945-947` | `RE::VTABLE_ActorMagicCaster[0]` slot **0x0A** (`CheckCast`) + 14× `VTABLE_CombatMagicCaster*[0]` slot **0x06** (`CheckStartCast`) | pre-charge deny + advisory cast pacing/friendly-fire | chainable vtable hooks | **NAMED both** — `MagicCaster.h:55` (0x0A, same slot as row 2 — MFO and APMF currently both hook it; see §5) and `CombatMagicCaster.h:27` `virtual bool CheckStartCast(CombatController*); // 06` | ID-backed (both) | N — covered, no PR |
| 9 | MFO | `native/CombatStyle.cpp:237,387-389` | 30× `VTABLE_CombatInventoryItemMagicT_...[0]`, slot **0x0F** | equip gate (#75) — same slot as row 3 | chainable vtable hook | **NAMED** — same citation as row 3 | ID-backed | N — covered, no PR (duplicate of APMF's own T2a — §5) |
| 10 | MFO | `native/MainThread.cpp:56-73,95-98` | `RE::VTABLE_PlayerCharacter[0]`, slot **0xAD** | `PlayerCharacter::Update` (the main-thread pump seat) | chainable vtable hook | **NAMED** — same citation as row 1 | ID-backed | N — covered, no PR |
| 11 | MFO | `native/Board.cpp:1847-1852,2357-2360` (`D3DInitHook` 1502, `DXGIPresentHook` 1580, `InputDispatchHook` 1634) | 3× mid-function CALL-instruction patches via `SKSE::GetTrampoline().write_call<5>` at `REL::RelocationID(75595,77226)`, `RelocationID(75461,77246)+0x9`, `RelocationID(67315,68617)+0x7B` | ImGui overlay: D3D device init, DXGI Present, input-event dispatch | **call-site trampoline** (not a vtable) | N/A — CommonLib doesn't "bind" a byte offset mid-function; nothing to name | ID-backed (all three already resolve via `RelocationID`, no gap) | **N — not upstreamable by kind** (overlay/call-site patch, matches the standing project note that trampolines are never CommonLib PR material, independent of naming) |

Not a hook site (checked and ruled out): `native/core/NativeBitProbe.cpp` only toggles
`Actor::BOOL_FLAGS` bits via the ordinary `GetActorRuntimeData().boolFlags` accessor — no
`write_vfunc`/`write_call` anywhere in the file. `native/Sightline.cpp:143`'s `RELOCATION_ID`
mention is a comment documenting what `TESObjectREFR::HasLineOfSight` (an ordinary already-bound
CommonLib member call) resolves to internally — MFO never hooks it, just calls it.

---

## 2. CommonLib binding requests (stream 1 — file at `CharmedBaryon/CommonLibSSE-NG`)

**Exactly one request.** Row 4 above is the only hooked site whose CommonLib binding is absent.
Row 6 (`EvaluatePackage`) is a hygiene note, not a request — the binding already exists. No other
row needs anything from this stream.

### Request: bind `CombatBehaviorTreeNode` (+ the `CombatBehaviorTreeControl`/
`CombatBehaviorController` pass-through structs it needs to be usefully typed)

**What's missing vs. what already exists — precisely:** the raw addresses are NOT missing. The
pinned tree already carries, in `namespace RE` inside `Offsets_VTABLE.h`/`Offsets_RTTI.h`:

- `VTABLE_CombatBehaviorTreeNode` (`Offsets_VTABLE.h:3426`, `REL::VariantID(265775, 212199, 0x1715610)`)
- `RTTI_CombatBehaviorTreeNode` (`Offsets_RTTI.h:1994`, `REL::VariantID(686393, 394204, 0x1efcf30)`)
- All 70 `VTABLE_CombatBehaviorTreeNodeObject_<Leaf>_` + matching `RTTI_CombatBehaviorTreeNodeObject_<Leaf>_` symbols (e.g. `Offsets_VTABLE.h:3723` `CombatBehaviorAttack` = `REL::VariantID(266747, 213789, 0x17216a0)`, `Offsets_RTTI.h:4114` same ids) — independently verified byte-for-byte identical to the triples `native/core/CombatBehaviorRE.h:105-176` currently re-declares locally from a DIFFERENT fork (`alandtse/CommonLibSSE-NG` @ CPR's pinned commit `3f9fc679...`), confirming these are stable facts about the compiled game, not fork-specific guesses.

What's genuinely absent is the **C++ class** that types those addresses: no
`CombatBehaviorTreeNode.h`/`CombatBehaviorTreeControl.h`/`CombatBehaviorThread.h`/
`CombatBehaviorController.h` exists anywhere in the pinned tree (confirmed by a full recursive
`git/trees?recursive=1` listing at the pinned SHA — zero paths match `*CombatBehavior*`). This
means the PR needs **no new Address Library ids at all** — it is purely "add the class over
addresses that already exist," the cleanest possible shape of PR.

**Proposed new file `include/RE/C/CombatBehaviorTreeNode.h`** (style matches
`CombatInventoryItem.h`/`MagicCaster.h`: tabbed member alignment, `// XX` slot comments, `RTTI =`
pattern):

```cpp
#pragma once

namespace RE
{
	class CombatBehaviorTreeControl;

	// Base class of every combat behavior-tree leaf/selector node (~70 concrete
	// leaves + selectors, VTABLE_CombatBehaviorTreeNodeObject_* in
	// Offsets_VTABLE.h). Layout reversed by CombatPathingRevolution
	// (src/RE/CombatBehaviorTreeNode.h, alandtse/CommonLibSSE-NG); cross-checked
	// and field-proven (deny via act()'s own vtable slot, ForceFail path) by
	// AI Package Management Framework, 2026-09-03, 1.6.1170.
	class CombatBehaviorTreeNode
	{
	public:
		inline static constexpr auto RTTI = RTTI_CombatBehaviorTreeNode;

		virtual ~CombatBehaviorTreeNode();  // 00

		virtual const char*               GetName() const;                                       // 01
		virtual CombatBehaviorTreeControl* Act(CombatBehaviorTreeControl* a_control);              // 02 - "Enter"; the node's own decision/action
		virtual CombatBehaviorTreeControl* Pop(CombatBehaviorTreeControl* a_control);               // 03
		virtual CombatBehaviorTreeControl* OnChildFailed(CombatBehaviorTreeControl* a_control);     // 04
		virtual CombatBehaviorTreeControl* OnInterrupted(CombatBehaviorTreeControl* a_control);     // 05
		virtual void                       SaveGame(CombatBehaviorTreeControl* a_control, void* a_buffer);  // 06
		virtual void                       LoadGame(CombatBehaviorTreeControl* a_control, void* a_buffer);  // 07
		virtual bool                       Unk_08(CombatBehaviorTreeControl* a_control);            // 08
		virtual const BSFixedString*       Unk_09() const;                                          // 09

		// members
		BSFixedString                     name;          // 00
		CombatBehaviorTreeNode*           parent;        // 10
		CombatBehaviorTreeNode**          childs;        // 18
		std::uint32_t                     numChilds;     // 20
	};
	static_assert(sizeof(CombatBehaviorTreeNode) == 0x28);
}
```

Note on confidence: slots `00`–`03` (dtor, `GetName`, `Act`, `Pop`) and the `sizeof == 0x28`/member
layout are the ones APMF's ch.7 (`ActionGate.cpp`) actually exercises and field-proved (deny via
invoking `CombatBehaviorForceFail`'s own original `Act()`, not a hand-reconstructed call — see
`Docs/ALLOWANCE-TEMPLATE.md` §7's field-proof note). Slots `04`–`09` are carried over from CPR's
header for completeness but are **not independently field-verified by this project** — flag this
in the PR body so a reviewer doesn't read "field-proven" as covering the whole vtable.

**Proposed new file `include/RE/C/CombatBehaviorTreeControl.h`** — deliberately MINIMAL. This is
the opaque object the engine passes into `Act()`; only one field is field-verified (2026-09-03,
1.6.1170 deck run — see `Docs/ALLOWANCE-TEMPLATE.md` §5's resolved layout-ambiguity note), so the
PR should NOT claim a full reversed layout:

```cpp
#pragma once

namespace RE
{
	class CombatBehaviorController;

	// Opaque per-thread control object passed to CombatBehaviorTreeNode::Act().
	// Non-polymorphic (no vtable/RTTI symbol exists in the pinned tree, and none
	// is expected for a plain data object). Layout below is DELIBERATELY
	// PARTIAL: only the one field below is field-verified; the object is larger
	// (CombatPathingRevolution's own header suggests headers well past 0x158)
	// but the rest is unreversed here.
	class CombatBehaviorTreeControl
	{
	public:
		std::byte                 unk000[0x158];       // unreversed
		CombatBehaviorController* controller;           // 158 - field-verified 2026-09-03, 1.6.1170 (AI Package Management Framework)
	};
}
```

**Proposed new file `include/RE/C/CombatBehaviorController.h`** — same minimal-and-honest shape,
one field verified:

```cpp
#pragma once

namespace RE
{
	class CombatController;

	// One hop off CombatBehaviorTreeControl::controller (+0x158). Field-verified
	// 2026-09-03, 1.6.1170: the +0x20 member here IS RE::CombatController* on
	// this runtime (NOT +0x158 of CombatBehaviorTreeControl directly, which is
	// CombatPathingRevolution's own — different — struct's typing and resolves
	// null on this runtime; see AI Package Management Framework's
	// Docs/ALLOWANCE-TEMPLATE.md §5 for the two-hypothesis field record).
	class CombatBehaviorController
	{
	public:
		std::byte        unk00[0x20];    // unreversed (combatGroup/state/inventory/blackboard, unconfirmed order)
		CombatController* combatController;  // 20 - field-verified 2026-09-03, 1.6.1170
	};
}
```

**Proposed PR title:** `Add CombatBehaviorTreeNode class binding (addresses already exist in Offsets_VTABLE.h/Offsets_RTTI.h)`

**One-paragraph PR description:** CommonLibSSE-NG's `Offsets_VTABLE.h`/`Offsets_RTTI.h` already
carry the vtable and RTTI addresses for `CombatBehaviorTreeNode` and its ~70 concrete leaves
(`VTABLE_CombatBehaviorTreeNodeObject_*`) — every SE+AE Address Library id this PR needs is
already present — but no header exposes them as a typed C++ class, so any plugin that wants to
observe or influence combat AI decisions (the single most generic "what is this NPC about to do"
interface the engine has — one base vtable, ~70 leaf instantiations covering attack/block/dodge/
cast/equip/flee/search/movement) has to hand-roll a local RE:: extension header, as
CombatPathingRevolution and (this PR's source) the AI Package Management Framework project both
independently did. This PR adds that header, field-proven against a live 1.6.1170 deck run
(vtable dispatch for real actors confirmed; `SetFailed`-equivalent deny via the leaf's own
original `Act()` confirmed to work with no CTD and no re-entry storm) — deliberately scoped to the
slots and layout actually exercised in the field, with everything else left as explicit
unreversed padding rather than guessed.

---

## 3. Address Library ID requests (stream 2 — the versionlib id database)

**None.** Every site in the coverage table above resolves its address via an existing
`REL::VariantID`/`REL::RelocationID`/`RE::VTABLE_*` triple — including row 4's `Offsets_VTABLE.h`/
`Offsets_RTTI.h` entries, which are already there even though no class binds them (§2). Neither
repo uses a raw hardcoded offset or a signature scan anywhere in the hook/trampoline sites
enumerated in §1.

One historical near-miss, noted for completeness and NOT a current gap: `Docs/
ALLOWANCE-TEMPLATE.md` records that `CombatBehaviorTreeControl::SetFailed`/`Ascend` (the function
`CombatBehaviorForceFail`'s body calls internally) has **only a published SE Address Library id
(46240/46229), no AE id anywhere this project can reach**. This mattered for the REMOVED
`T1Probe.cpp`, which worked around the missing AE id by disassembling `ForceFail`'s own compiled
`Act()` body at runtime for its first CALL instruction rather than resolving `SetFailed` by
address at all. The GRADUATED, currently-shipping site (`ActionGate.cpp:176-181`) doesn't call
`SetFailed` by address either — it recovers and invokes `ForceFail`'s own original `Act()` from
its own `write_vfunc` install map (row 4's hook, already ID-backed). So there is nothing to submit
today; flagging only so a future maintainer doesn't rediscover this from scratch. If someone later
wants to call `SetFailed` directly (rather than through `ForceFail::Act()`), a genuine AE id gap
would need filling — but that isn't a request this project can make, since we don't have the id
either (would require a runtime dumper, not a header read).

---

## 4. Existing PR overlap check (#107, #108, #109)

Fetched via `gh pr view <n> --repo CharmedBaryon/CommonLibSSE-NG`, all three `OPEN`:

| PR | Title | Touches |
|---|---|---|
| #107 | Add binding for `Actor::StartCombat` | `include/RE/A/Actor.h`, `src/RE/A/Actor.cpp` |
| #108 | Define `ExtraDataList` default constructor (declared but unlinkable) | `src/RE/E/ExtraDataList.cpp` |
| #109 | Add binding for `SendInventoryUpdateMessage` | `include/RE/M/Misc.h`, `src/RE/M/Misc.cpp` |

**No overlap.** None of the three touches combat behavior-tree classes, `MagicCaster`,
`CombatInventoryItem`, `CombatMagicCaster`, `TESObjectREFR`/`Actor`'s package-selection vfuncs, or
`BGSProcedureTreeProcedure`. The §2 request is a genuinely new, fourth PR.

---

## 5. Flagged, not ready — `BGSProcedureTreeProcedure`'s `Unk_XX` slots

**This is the "non-alias package slot under investigation" from marth's sharpener** — the site
`Docs/PROBE-NONALIAS-PACKAGE.md` names as the closest structural analogue to a per-procedure
"should this activate" decision (the thing that would let APMF quietly hold a package like
Cicero's `0009BE51` instead of re-asserting every tick). Confirmed again here directly:

- `include/RE/B/BGSProcedureTreeProcedure.h` (full file read, pinned commit): a real, polymorphic
  17-slot vtable — `~BGSProcedureTreeProcedure() override; // 00`, `Load(TESFile*) override; // 03`,
  and **every other slot (`01`,`02`,`04`–`10`) is `void Unk_XX(void) override;`** — genuinely
  undocumented, not a naming oversight.
- Class-level addresses ARE already covered: `VTABLE_BGSProcedureTreeProcedure`
  (`Offsets_VTABLE.h:1478`, ids `253756`/`203470`) and `RTTI_BGSProcedureTreeProcedure`
  (`Offsets_RTTI.h:1579`, ids `685992`/`393790`) both already exist. So — same shape as §2 — an
  eventual PR would need **zero new Address Library ids**, purely a class binding.

**Why this is flagged, not PR'd:** every `Unk_XX` slot's real signature and semantics are
genuinely unknown (`void(void)` is CommonLib's placeholder convention, not a verified ABI). A
wrong guess on a blind vtable slot in a header PR is silent ABI corruption for every downstream
consumer, not a benign no-op — this is exactly the discipline `Docs/ALLOWANCE-TEMPLATE.md` and
`Docs/PROBE-NONALIAS-PACKAGE.md` both already insist on ("RTTI-verified, known signature" or don't
hook it). **Before any PR here, this needs the runtime dumper's confirmation** — a disassembly
trace of a live `BGSProcedureTreeProcedure` instance's vtable (IDA/Ghidra against the compiled
`.exe`, not available in this header-only research pass) to name the real per-slot signature. This
is its own follow-up project, not a diff this report can respons­ibly hand over.

---

## 6. Code-hygiene notes (not upstream requests — filed here so they aren't lost)

- **`PackageGate.cpp:78-83`** re-declares `Actor::EvaluatePackage`'s relocation locally
  (`RELOCATION_ID(36407,37401)`) when CommonLib already fully implements this as a callable member
  (`Actor.h:519` declares it, `Actor.cpp:249` defines it against the identical ids). Calling
  `a_actor->EvaluatePackage(true, false)` directly would drop the local re-declaration with no
  behavior change. Not upstream-relevant; a local simplification for whoever next touches
  `PackageGate.cpp`.
- **Rows 2/8 and 3/9 are the same CommonLib slots hooked twice**, once by APMF (`CastGate.cpp`
  T2c, `EquipGate.cpp` T2a) and once by MFO (`CasterConsent.cpp`'s `CheckCast` hook,
  `CombatStyle.cpp`'s equip gate). This is exactly what `feat/apmf-cast` commit `f80021f` ("owned
  cast becomes a pure APMF T2 client -- drop redundant enforcement") is in the middle of
  resolving — not a CommonLib gap, just noted here so the two-repo hook inventory is honest about
  the current overlap while that migration is mid-flight.

---

## Evidence file index

- `native/core/Allowance.h`, `Allowance.cpp` (APMF) — the shared `InstallOnVtables`/`Allowed`
  template every T2/T1/T3 site rides.
- `native/core/{Hook,CastGate,EquipGate,ActionGate,PackageGate}.cpp`, `native/core/
  CombatBehaviorRE.h`, `native/core/NativeBitProbe.cpp`, `native/plugin.cpp` (APMF).
- `native/{Targeting,CasterConsent,CombatStyle,MainThread,Board,Sightline}.cpp` (MFO).
- `Docs/ALLOWANCE-TEMPLATE.md`, `Docs/PROBE-NONALIAS-PACKAGE.md` (APMF) — prior research this
  spike re-verified against live headers rather than re-deriving.
- Pinned-commit fetches (all via `raw.githubusercontent.com/CharmedBaryon/CommonLibSSE-NG/
  c4ab853d.../...`): `include/RE/Offsets_VTABLE.h`, `include/RE/Offsets_RTTI.h`,
  `include/RE/M/MagicCaster.h`, `include/RE/C/CombatInventoryItem.h`,
  `include/RE/C/CombatMagicCaster.h`, `include/RE/A/Actor.h`, `include/RE/A/ActorMagicCaster.h`,
  `include/RE/B/BGSProcedureTreeProcedure.h`, `src/RE/A/Actor.cpp`; plus a full recursive
  `git/trees?recursive=1` listing at the same SHA for file-existence checks.
- `gh pr view {107,108,109} --repo CharmedBaryon/CommonLibSSE-NG` — existing-PR overlap check.
