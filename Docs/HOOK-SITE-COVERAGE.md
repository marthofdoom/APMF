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

> **UPDATE 2026-09-07 — THE "ZERO RAW OFFSETS" BOTTOM LINE BELOW IS NO LONGER TRUE.**
> The inventory was accurate on 2026-09-03 and has not been re-run since; three sites added
> after it are missing from the table, and two of them are exactly the case this document
> exists to flag:
> - **`native/core/CastSeats.cpp`** — the ch.8b engine seats: slots 0x06/0x07/0x0A/0x0D on
>   `VTABLE_CombatMagicCasterRestore[0]` **and** `VTABLE_CombatMagicCasterOffensive[0]`.
>   ID-backed, named slots; would be "covered, no PR". NOTE `GetMagicTarget` (0x0A) has a
>   hidden 16-byte sret out-slot CommonLib's declaration OMITS — an ABI defect, not a naming
>   gap, and a genuine upstream item.
> - **`native/core/CastClassify.cpp`** — `VTABLE_CombatMagicItemData` slot 1, verified at
>   install by an **RTTI type-name string**. (CORRECTED 2026-09-15: the pinned tree DOES carry
>   `RTTI_CombatMagicItemData{687623, 395938}`; the "no id" reading was wrong. The string match
>   stays. The three raw member offsets it reads are per-binary literals — see §1a.)
> - **`native/core/AiCastSeats.cpp` GROUP C** — the four weapon-class `CalculateScore` (0x0C)
>   leaves. (CORRECTED 2026-09-15: they were resolved from raw AE RVAs because the 2026-09-06
>   pass believed no Address-Library id existed; the pinned tree carries
>   `VTABLE_CombatInventoryItem{Melee,Ranged,Shield,Torch}` and GROUP C now resolves through
>   them. The install gate's slot-0x0C EXPECTED VALUE is still a per-binary literal — see
>   §1a. There is nothing left to submit to the Address Library for this site.)
>
> Re-run this inventory before trusting its counts, and before any new-runtime port that
> plans around "no raw offsets to re-verify".

**Bottom line up front (2026-09-03, STALE — see the update above):** of 10 hooked sites across
both repos, **9 are already fully covered** (named CommonLib slot + existing Address Library id). **Exactly one CommonLib gap is PR-ready**
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
| 12 | APMF | `native/core/EquipSink.cpp` (`Install`, `SinkThunk`) | **2 mid-function CALL-instruction patches per runtime** via `SKSE::GetTrampoline().write_call<5>`: AE `REL::ID(38894)+0x170` (RVA 0x6C9990, bytes `E8 9B 24 00 00`) and `REL::ID(38893)+0xBC` (0x6C97EC, `E8 3F 26 00 00`); SE `REL::ID(37938)+0xE5` (0x637B65, `E8 B6 22 00 00`) and `REL::ID(37937)+0xBC` (0x637A4C, `E8 CF 23 00 00`) — the only two callers of the `ActorEquipManager` worker AE 38929 (0x6CBE30) / SE 37974 (0x639E20), full E8/E9/lea/pointer scan 2026-09-15 | the engine-equip SINK (ch.17 `kIntent_EquipAuthority`): deny = do not call the worker | **call-site patch — the ONE bounded exception, INVARIANTS #17a** (no virtual exists on the path; both sites byte-verified as `E8 rel32 → worker` before either is written, any mismatch refuses the whole seat). Caller classified by reading `EquipObject`'s caller's return slot at a per-site, per-runtime depth measured from the bytes: AE 38894 `[rsp+0x80]` (5 pushes + `sub rsp,0x50`), AE 38893 `[rsp+0x70]` (3 pushes), SE 37938 `[rsp+0x60]` (`push rdi`), SE 37937 `[rsp+0x70]` (3 pushes) | **NOT bound** — CommonLib binds only the public `ActorEquipManager::EquipObject`; the worker and the `EquipData` struct it takes (`{extra@0,count@8,slot@0x10,0@0x18,queue@0x20,force@0x21,sounds@0x22,applyNow@0x23}`) are local RE | ID-backed on both runtimes (ids above; offsets are APMF's own, from the disassembly) | N — a call-site patch is not a binding request; the worker's signature could be filed as an RE note |
| 13 | APMF | ~~`native/channels/Travel.cpp` (`Poll`)~~ **REMOVED from v1** | `Actor::HasLineOfSight(TESObjectREFR*, bool&)` call (was the ch.19 "perceived" test; the end conditions are now ARRIVAL, `IsInCombat` and destination-gone, so this call is NOT MADE by any APMF code) | not a vtable/RTTI site -- a plain non-virtual member call, resolved by CommonLib's own `RELOCATION_ID(53029, 53829)` | non-virtual function call through CommonLib's binding | **Already fully bound in CommonLib** -- `Actor.h:563` declares it, `src/RE/A/Actor.cpp:648` defines it against those ids | ID-backed (already CommonLib's own; both ids re-resolved against the unpacked images 2026-09-22, byte-identical prologues -- `Docs/ADDRESS-TABLE-2026-09-15.md` ch.19 addendum) | N -- not a gap. **NO VR id exists**, which is one of the reasons ch.19 is VR-refused at install |
| 14 | APMF | ~~`native/channels/Travel.cpp` (`Poll`)~~ **REMOVED from v1** | `Actor::RequestDetectionLevel(Actor*, DETECTION_PRIORITY)` call (was the ch.19 second "perceived" test; removed with it, and it was an UNOBSERVED path for this project either way) | not a vtable/RTTI site -- a plain non-virtual member call via `Offset::Actor::RequestDetectionLevel` | non-virtual function call through CommonLib's binding | **Already fully bound in CommonLib** -- `Actor.h:608` declares it, `include/RE/Offsets.h:17` carries `RELOCATION_ID(36748, 37764)` | ID-backed (already CommonLib's own; both ids re-resolved 2026-09-22, byte-identical prologues) | N -- not a gap |

| 15 | APMF | `native/channels/Travel.cpp` (`Poll`) | `Actor::IsInCombat()` call — the ch.19 combat-cancel test, a VIRTUAL call, not a hook | `RE::VTABLE_Character[0]` slot **0xE3** dispatched through CommonLib's `RelocateVirtual` (VR 0xE5, never reached: ch.19 is VR-refused) | chainless virtual call; APMF installs nothing on this slot | **NAMED** — `Actor.h:421` `SKYRIM_REL_VR_VIRTUAL bool IsInCombat() const; // 0E3`, `src/RE/A/Actor.cpp:1558` | vtable-index based, so no Address-Library id is needed. Body verified on BOTH unpacked images 2026-09-22: SE `0x625660` (addrlib 37609) / AE `0x6B6DD0` (addrlib 38562), instruction-for-instruction identical apart from the `combatController` member offset (SE `+0x158`, AE `+0x160` — the AE +8 shift, which APMF never touches itself because the call goes through the vtable) | N — not a gap, and not a hook |

**ch.19 (`kIntent_Travel`, ABI v10) ADDS NO HOOK SITE.** It is recorded here anyway, because "which sites exist" is the question this document answers and a NEW CHANNEL THAT ADDS NO SITE is exactly the answer a reader would otherwise have to go and prove for themselves. ch.19 files one internal ch.9 claim (row 5's 0x49 seat + row 6's `EvaluatePackage` nudge) and runs its leg monitor on `Arbiter::OncePerFrame`, i.e. row 1's existing `VTABLE_PlayerCharacter[0]` 0xAD seat. The `Character::UpdateCombat` 0xE4 seat that an earlier ch.19 design called for (mirroring MFO's row 7) was CUT from v1 by marth on 2026-09-22 and is NOT installed by APMF; target pinning is becoming its OWN future facet, not a v2 of this one.

### 1a. Per-runtime placement of APMF's single-runtime constructs (2026-09-15)

APMF `native/` has **zero** `REL::ID(` sites (CLAUDE.md rule 11's old "six single-version
`REL::ID` sites" sentence never described a `REL::ID` count). What it has is six MECHANISMS
that carry a per-binary fact — a raw member offset, a raw slot index, a per-binary expected
function value, or a log label. This table is their placement state per runtime, sourced from
the 2026-09-15 CONFIRMED address table (re-derived cell by cell against the 1.5.97 and 1.6.1170
address libraries + unpacked binaries; 1.7.104 static analysis only). "open" = the runtime gate
admits the binary; "no gate" = the construct is layout-identical by evidence and runs ungated;
"gated" = refused with a loud line.

| # | Site | Construct | 1.6.1170 | 1.5.97 | 1.7.104 | Gate | CONFIRMED-table row |
|---|---|---|---|---|---|---|---|
| A | `native/core/CastClassify.cpp` (`kSpellOffset/kCtrlOffset/kSelfFlagOffset`, `Install()`) | `CombatMagicItemData` +0x10/+0x18/+0x4c, slot 1 thunk | **open** (thunk `0x81D830`) | **open** (thunk `0x7811F0`, id 43931; ctor `0x780F5C` stores the same three fields) | **open** (F2b) — thunk `0x832D20`; ctor `0x832A90` and the visitor identical to 1.6.1170, KeepBestRow 45325 -> `0x833090`, classify table rows identical | exact version `1.6.1170 \|\| 1.5.97 \|\| 1.7.104` (NOT `IsAE()/IsSE()` — 3.7.0's `IsSE()` is the `default:` arm); the real wrong-id guard is the RTTI mangled-name compare | "CastClassify.h SEAT 0", slot-1 row |
| B | `native/core/AiCastSeats.cpp` GROUP C (`kWeaponClasses`, `Install()`) | vtables via `VTABLE_CombatInventoryItem{Melee,Ranged,Shield,Torch}[0]` (SE 264523/264525/264527/264531, AE 210297/210299/210301/210305); slot-0x0C expected value per runtime | **open** — expected `0x8183e0/0x8188b0/0x818df0/0x819480` | **open** — expected `0x77e0a0/0x77e550/0x77eac0/0x77f0e0` (Shield/Torch are 0xF-byte arg-swap thunks; the slot value is still the class's own entry) | **open** (F2b) — expected `0x82D2C0/0x82D790/0x82DCD0/0x82E360` (`calcScoreRva17`), each the 1.6.1170 body, identical | same exact-version predicate as A (inlined per file, not a shared helper); the real wrong-id guard is the slot-0x0C expected-value compare per class (a `vt.address()==0` test precedes it as unreachable belt-and-braces — 3.7.0 never nulls a missing id) | "Group C: weapon-class CalculateScore seats" (8 rows) |
| B2 | `native/core/AiCastSeats.cpp` TASK2 (`EnableDualWieldPreference`, inside the Group C loop) | Shield vtable slot 0x0F `CheckShouldEquip`, `ShieldEquip_t = bool(CombatInventoryItem*, CombatController*)` | **open** — slot 0x0F `0x817FC0` on vtable `0x18C9188` (21 slots) | **open** — slot 0x0F `0x77DC90` on vtable `0x1681C28` (21 slots); body `sub rsp,0x28; mov rcx,rdx; call 0x4FDE10; test al,al; sete al; add rsp,0x28; ret`, byte-shape-identical to AE | **open** (F2b) — slot 0x0F `0x82CEA0`, identical | rides B's gate + B's 0x0C identity pass for the Shield entry; INI default 0 | "Group C", Shield `CheckShouldEquip (slot 0x0F)` row (added post-confirmation, reviewer re-derived) |
| C | `native/core/CastSeats.cpp` (`kAimTargetOverride`) | `CombatAimController` +0x30 aim override | no gate | no gate — ctor zeroes `[+0x30]`, vfunc7 reads it first | **open** (F2b) — aim ctor `0x809EE0` identical (`[+0x28]`, `[+0x30]`) | construct ungated; the `castseats::Install` that reaches it is exact-version gated since G1 (2026-10-04); INI switch + install RTTI + per-call vtable identity (unchanged) | "MFO layout facts", CombatController row (c) |
| D | `native/core/CastSeats.cpp` / `native/core/AiCastSeats.cpp` (`Out16`) | `GetMagicTarget` hidden sret `{u32 @0, ptr @8}` | no gate (`0x81e020`) | no gate (`0x781CB0` + helper `0x782100`) | no gate (same ABI) | none needed | "GetMagicTarget sret" row |
| E | `native/core/EquipGate.cpp` (`CallSiteName`) | 0x0F call-site LABEL table (`0x80fcd0`, `0x813af2/0x813d38/0x814270/0x8144b2`) | consulted | not consulted — prints the live RVA as "unlabelled" | consulted (F2b) — `0x824bb0`, `0x8289d2/0x828c18/0x829150/0x829392` | exact version `1.6.1170` / `1.7.104`; cosmetic, nothing gates on it. NOTE: these are the `call` instructions (and 44868's start), not return addresses, so they never match `_ReturnAddress()`; pre-existing, log-only | none (SE call sites were not derived) |
| F | `native/core/NonAliasProbe.cpp` (`kPutCreatedPackage`) | `Character` vtable slot 0xDF | no gate | no gate — 298-slot vtable, same 12-callee function id-for-id | **open** (F2b) — slot 0xDF `0x691590`, identical (202 insns) | exact version `1.6.1170 \|\| 1.5.97 \|\| 1.7.104` (F2b) since G1 (2026-10-04, `allowance::RuntimeSupported()`); was VR-refused only | `Targeting.cpp:162` `VTABLE_Character` row |

Also confirmed on 1.5.97 with no change needed (RTTI-verified at install, no version gate): the
14 `CombatMagicCaster` seat vtables (`CastSeats.cpp`, `AiCastSeats.cpp` GROUP B), the 30
`CombatInventoryItemMagicT` combos incl. the two `_CombatMagicCasterArmor_` rows
(`EquipGate.cpp`, `AiCastSeats.cpp` GROUP A), the `<0x68` `CombatController` reads
(`attackerHandle` 0x28 / `targetHandle` 0x2C / `combatStyle` 0x38 confirmed by direct read in
the table's CombatController row; `inventory` 0x10 is CommonLib-declared and < 0x68, i.e. in the
unshifted region per ENGINE_NOTES §0.29, not a table cell — APMF reads nothing above 0x68), and
the 72 `CombatBehaviorTree` leaf triples (`ActionGate.cpp`). `plugin.cpp` logs
`[runtime] <version>: SUPPORTED|NOT SUPPORTED ... cast-classify <open|gated>, group-C <open|gated>, equip-sink <open|gated>` once at load.
**G1 (2026-10-04):** every seat that used to be VR-refused only is now refused on any build that
is not exactly 1.6.1170 or 1.5.97 (`allowance::RuntimeSupported()` in `core/Allowance.h`), so no
build reaches a 1.6 slot, offset or id through the `IsAE()`/`IsSE()` buckets. F2b (2026-10-05) adds
exactly 1.7.104 to that predicate.

**1.7.104 (F2b, 2026-10-05): every APMF seat is placed and open.** The CommonLibSSE-NG fork reads
its own MIT id table on 1.7.104 (keyed by the 1.6.1170 id) and files 1.7.x in the AE column, so every
`RELOCATION_ID` resolves to its AE id there. `RuntimeSupported()` is exactly 1.6.1170, 1.5.97 or
1.7.104; the per-runtime literals carry their own 1.7.104 values (rows A-F above, plus EquipSink's AE
tables and ch.22's self-check offset +0x8F). Each was proven on the 1.7.104 binary, and the generated
`VerifiedAddresses.h` 1.7.104 table re-verifies every address at load against the table the game actually
read (`Docs/VERIFIED-ADDRESSES.md`, including its "1.7.104 proof" section). The 1.7.104 column is
documented per seat in the table below. No seat is refused on 1.7.104.

| Seat | File | 1.7.104 | Proof (1.6.1170 -> 1.7.104) |
|---|---|---|---|
| 0xAD arbiter (Character / PlayerCharacter) | `core/Hook.cpp` | open | `0x667D40 -> 0x67A7E0`, `0x732660 -> 0x745200`: same signature and frame |
| ch.9 0x49 (CheckForCurrentAliasPackage) | `core/PackageGate.cpp` | open | `0x66CB60 -> 0x67F620` identical; EvaluatePackage 37401 identical |
| 0xDF observe (PutCreatedPackage) | `core/NonAliasProbe.cpp` | open | `0x67EAD0 -> 0x691590` identical |
| CheckCast 0x0A (ActorMagicCaster) | `core/CastGate.cpp` | open | `0x5B1610 -> 0x5C0150` identical |
| CheckShouldEquip 0x0F (30 MagicT vtables) | `core/EquipGate.cpp` | open | 30 slot functions identical; hand slots via the fork's DOBJ `GetObject` |
| ch.7 leaves act/pop/update (70 vtables) | `core/ActionGate.cpp` | open | every slot function identical |
| ch.8b 0x06/0x07/0x0A/0x0D (Restore, Offensive) | `core/CastSeats.cpp` | open | identical; aim ctor `0x7F51C0 -> 0x809EE0`; GetMagicTarget `0x81E020 -> 0x833510` |
| ch.8b seat 0 classify | `core/CastClassify.cpp` | open | row A |
| AiCastSeats A (0x0C/0x0F), B (0x06/0x07/0x0A), C (0x0C), ranged probe | `core/AiCastSeats.cpp` | open | slot functions identical; row B / B2 values |
| caster census 0x06/0x0B | `core/CasterTypeCensus.cpp` | open | 15 caster vtables' slot functions identical |
| ch.17 equip sink (+0x170 / +0xBC E8 -> worker) | `core/EquipSink.cpp` | open | EquipObject 38894, sibling 38893, worker 38929 identical; 39 caller sites in the same path functions |
| ch.20 selector 0x06 + Character 0xE4 | `channels/TargetPin.cpp` | open | `0x84CFE0 -> 0x862040`, `0x84DAF0 -> 0x862B50`, `0x6B6E70 -> 0x6C98F0` identical |
| ch.22 Character 0x99 + StartCombat +0x8F | `channels/CombatReentryDeny.cpp` | open | `0x674ED0 -> 0x6879A0` identical; StartCombat 38561 identical |
| ch.21 StartCombat call | `channels/CombatEntry.cpp` | open | 38561 identical |
| ch.12 PlayIdle + graph confirmation | `channels/Idle.cpp` | open | 39256 / 39257 identical; graph classes unchanged |
| ch.19 travel, gait, gate probe | `channels/Travel.cpp` | open | gait path and probe calls identical |
| ch.1 movement calls | `channels/MovementDeny.cpp` | open | 37489 / 37894 / 37895 identical |
| v11 position cast / space queries | `core/PositionCast.cpp`, `core/SpaceQuery.cpp` | open | 13723 / 13322 / 34408 / 37537 and slots 0x23/0x5C/0x89, NonActorMagicCaster 1, bhkWorld 0x33 identical |

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
enumerated in §1. (2026-09-15: GROUP C's vtables, the one exception the 2026-09-07 update box
flagged, now resolve through the pinned `VTABLE_CombatInventoryItem{Melee,Ranged,Shield,Torch}`
ids — see §1a. The per-binary literals that remain are member offsets and expected-value
checks, which the Address Library does not carry.)

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
  cast becomes a pure APMF T2 client -- drop redundant enforcement") set out to resolve — not a
  CommonLib gap, noted here so the two-repo hook inventory is honest about the overlap.
  **STATUS 2026-09-07: that migration never finished, and nothing recorded it.** MFO's
  `CasterConsent.cpp` still installs its own 0x0A `CheckCast` hook (thunk `:917`, install `:1054`), so the double hook
  is live today — outside APMF's gate, and able to abort a cast APMF's own claim is driving
  (MFO `DIAG-2026-09-06-deny-heal-failures.md` row P12: LATENT, 0 hits that session; latent is
  not closed). `Docs/SPEC-GRADUATED-CAST.md` §3 carries the unmeasured consequence.

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
