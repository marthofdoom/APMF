# CLAUDE.md — APMF / "Harbinger" (AI Package Management Framework), SKSE C++ plugin

APMF is a per-facet AI **moderator** for Skyrim NPCs: client mods declare intent,
APMF arbitrates who owns each facet, denies the losing sources, and composes the
result onto the actor — WITHOUT substituting the actor's package. MFO
(marth's Follower Overhaul, a separate repo) is its primary client and showpiece.

**Read first:** `design.md` (the model), `Docs/ARCHITECTURE.md`, `Docs/INVARIANTS.md`
(26 numbered rules — code cites them as `#N`; #0-#20 plus the sub-rules #3a/#3c/#5a/#14a/#14b.
There is no #3b: it left with `feat/alias-drive`, `Docs/SPEC-COMPOSITION-REWORK.md`), `Docs/CHANNEL-MAP.md` (channel ↔ hook
site), `Docs/DENY-COMPLETENESS-AUDIT.md`, `Docs/HOOK-SITE-COVERAGE.md`, `Docs/STATUS.md`
(living handoff — update it in the SAME change as any release/field-test change).

## Working rules

- **Navigate by `file:line`.** Grep to a symbol, read a narrow window — do NOT read
  whole files. Large files must never linger in context.
- **Compile is CI-ONLY** (`.github/workflows/native.yml`). NEVER claim "it builds" —
  verify with `gh run view <id> --json status,conclusion` = `success`. "It built" has
  been wrong before.
- **Delegate bulk file-reads to a subagent** and keep only its conclusion.
- **ONE complete brief per agent, run to completion.** Small greps/reads/edits INLINE;
  spawn an agent only for genuinely bulk work. One agent per build tree — two agents
  on one tree corrupt files. CHEAP model for workers (Sonnet; Haiku for trivial);
  reserve the expensive models for diff-REVIEWS, deep RE, and risky threading work.
- **Never read vendored code**, `build/`, or `.git/`.
- **Probes must be FULLY PASSIVE:** no hotkeys, no toggles. Always-on rate-limited or
  config-gated logging only (`Data/SKSE/Plugins/APMF.ini`, default OFF).

## The frozen external contract

`native/APMF_API.h` is **byte-shared with MFO's `native/APMF_API.h`** and is
**APPEND-ONLY**. MFO's copy must mirror this one EXACTLY (verify with `md5sum` on both).
Never add, reorder or retype a struct field; never reuse a retired flag bit (mark it
RESERVED); never renumber an Intent. A client built against an older ABI must degrade
cleanly — guard every newer vtable slot with an `abiVersion >=` check before calling it,
never read past the end of a shorter interface struct.

## What breaks the game — verify before touching

1. **Balanced act()/pop().** A behaviour-tree node's `act()` (vtable slot 0x02) PUSHES
   state; the runner calls that SAME node's `pop()` (slot 0x03). Substituting an act()
   whose push size differs from what the node's own pop() removes CORRUPTS THE THREAD
   DATA STACK — this was a months-live CTD. Deny in PAIRS.
2. **Combat-thread hooks.** The arbiter seat and the gates run on the combat thread.
   Never touch a client's actor lists from there; read FormIDs / atomic mirrors only.
   `mainthread::Post` is the only road to the true main thread for 3D/cell mutation.
3. **RCU ControlMap publish ordering.** A reader sees the old generation or the new one,
   never a tear. Anything that un-teaches state a live claim still depends on must be
   deferred until strictly AFTER `Publish()` — releasing inside `Drain()` pulls state out
   from under a claim the seats still see.
4. **Version-fragile offsets.** Every raw struct offset and vfunc index is AE-specific.
   Guard with a `static_assert` on the offset, an install-time RTTI check, a per-call
   vtable-identity test, and an INI kill-switch. Re-verify all of them on a new runtime.

> **REVIEWER MODEL (marth 2026-09-23): every review, spot check and field diagnosis in this file is done by an
> OPUS 5.5 agent (the Agent tool's `opus` model), replacing Fable.** Historical statements below about what
> Fable found are left as they were, because they are records of what happened. marth's own quoted words
> are left verbatim too. The standing review model is still the cheap one: the author names its 1-5
> uncertain spots and the reviewer answers only those, unless the change is co-save / ABI / a new engine
> seat / a TU split.

## SCOPE DISCIPLINE — the git system only catches regressions if nobody skips it

**Every regression this project has shipped recently came from UNREQUESTED SCOPE reaching the deck through
a review nobody actually performed.** These are hard rules, for the agent doing the work AND for whoever
dispatches and merges it.

### For the worker
1. **DO EXACTLY WHAT THE BRIEF ASKS. NOTHING ELSE.** No refactors, no file splits, no moving code between
   files, no renames, no "while I was in there" cleanups, no new files — unless the brief asks for them by
   name. If the work seems to *need* one, **STOP and report it**; do not do it and mention it afterwards.
   **This OVERRIDES the 2500-line rule above: crossing 2500 lines is a STOP-and-report, NOT a licence to
   split inside an unrelated task.** A split is its own brief and its own field cycle, because a "pure
   mechanical, CI-identical" move is exactly the change whose breakage only shows up in the field — and
   "CI-identical" is not a claim any TU split may assert, since CI proves it compiles, not that it behaves.
   (What actually happened, 2026-09-06 — CORRECTED 2026-09-06 after a transcript audit, because the first
   version of this note was WRONG in three ways and a rules file teaching a false lesson is worse than no
   rule. A loot task hit the 2500 cap, split 560 lines into a new file, and SAID SO in its commit message:
   it was obeying the rule as written. The coordinator then **DID run `git diff --stat` and DID see
   `Logistics_Loot_Equipment.cpp | 461 +++++`** — and wrote that very filename into the tag message sixteen
   seconds later. It still shipped. **The gate was not skipped; it RAN IN A MODE WHERE IT COULD NOT FIRE**,
   because the diffstat was scanned for the things the reviewer already expected (`interrupt|loose|quest`)
   rather than for ANOMALIES. And the split did NOT break anything: a later line-by-line audit found the
   contained-item eligibility path **byte-for-byte unchanged**. **THE LESSON IS NOT "read the diffstat" —
   it is "read it for what you did NOT expect."** A review that only confirms the story you arrived with is
   not a review. Grep it for `^[ADRC]` and for files outside the brief's stated scope, and paste that output
   verbatim before merging, tagging or deploying.)
2. **NEVER GUESS AN API OR A SYMBOL.** Verify it against the real CommonLibSSE-NG header/source or the
   disassembly before using it. "It compiles in my head" is not verification. (Real cost: two CI failures
   in one night on invented symbols — `ExtraDataList::HasQuestObjectAlias`, `EffectSetting::Data::Flag::kHostile`.)
3. **NEVER REPORT SUCCESS WITHOUT A VERIFIED-GREEN CI RUN.** Re-read the NEWEST run id, confirm its
   `headSha` is YOUR commit, and accept only `success`/`failure` (`cancelled`/`pending`/`queued` mean keep
   waiting — the workflow sets `cancel-in-progress`, so a newer run cancels older ones). A green run on
   the wrong SHA proves nothing about your code.
4. **DO NOT CREATE BRANCHES, TAGS, RELEASES OR INTEGRATIONS YOU WERE NOT ASKED FOR.** Push your own branch;
   that is all.
5. **STAY INSIDE YOUR FILE BOUNDARY.** If the brief lists files you own, touching anything else — even
   something obviously related — must be reported, not assumed.

### For whoever dispatches and merges
6. **READ THE ACTUAL DIFF BEFORE MERGING OR TAGGING. A SUMMARY IS NOT A REVIEW.** Run `git diff --stat`
   first and treat **any new file, deleted file, or large move as a STOP** until it is explained and
   justified. A branch whose diffstat does not match its brief has not been reviewed just because its CI
   is green — CI proves it compiles, not that it does what was asked.
7. **Never deploy a branch you have not personally diffed.** CI-green plus a plausible agent summary is
   exactly how an unreviewed refactor reaches the field.

## Engineering + design principles — HOW TO THINK HERE (read before designing anything)

These are hard-won, each one paid for with a crash, a wasted deploy cycle, or a
month-long bug. They apply to EVERY worker on this codebase, not just the author
of a given change.

1. **THINK WITH PORTALS (marth).** An engine STATE is a CONTAINER OF FACETS, never a
   monolith and never a cost to accept. When machinery you need only exists inside a
   state you don't want (e.g. the `CombatMagicCaster` set only exists with a live
   `CombatController`), do NOT avoid the state and do NOT work around it: force the
   state as a pure SUBSTRATE, open a portal for the ONE facet you came for, and DENY
   everything else it switches on. Combat becomes a substrate, not a mode. This
   generalizes: any "the engine only does X while in state Y" problem has this shape.
2. **DENY-COMPLETENESS LICENSES STATE-FORCING.** For EVERY facet a client can claim,
   there must be a COMPLETE deny — the competing source reduced to ZERO influence.
   That invariant is what makes principle 1 legitimate instead of a blunt hack: if
   nothing unrequested can get through, entering any state is safe. So when reviewing
   a design, NEVER ask "which side effects are acceptable?" — ask **"for each facet
   this switches on, do we have a complete deny, and WHERE ARE THE HOLES?"**
3. **COMPOSITION, NOT SUBSTITUTION.** Never swap in a whole package; a substituted
   package makes the preempted source lose its slot, fire OnPackageEnd and tear down
   (that is why substitution FREEZES the body). Moderate PER FACET. The NPC ends up
   running its own non-denied facets plus what we let through, while its real package
   keeps ticking.
4. **DECLARE → ENFORCE.** The client declares WHAT and WHERE; the framework enforces
   the ordered composition. Enforce ONLY what was declared — never fabricate un-given
   input. "Manufacturing the ordered composition" is allowed; inventing intent is not.
5. **DISASSEMBLY PROVES A PATH EXISTS, NOT THAT IT RUNS.** Before building on a code
   path, OBSERVE IT EXECUTING (a passive probe, a log line, a field capture). Cost of
   ignoring this: five engine seats were implemented, reviewed, CI-green and deployed
   onto `CombatMagicCasterRestore` — a caster the engine never runs (0 occurrences in
   a whole session vs 69 Offensive). Everything was correct except the assumption.
6. **COMMONLIB DECLARATIONS ARE NOT ABI-TRUSTWORTHY.** Verify every vfunc signature
   against the DISASSEMBLED target binary, never against CommonLib's header. Cost of
   ignoring this: a wrong `GetMagicTarget` signature (a hidden sret out-slot CommonLib
   omits) made a "passive" observation probe crash the game.
7. **NEVER MASK A FAILURE.** No watchdog, retry-fallback or safety net that makes a
   broken mechanism LOOK like it worked. An unmasked failure diagnoses in ONE field
   cycle; a masked one hides indefinitely and costs many. Log the failure loudly and
   let it fail. (Distinct from a legitimate DEGRADE path chosen by design — e.g.
   "framework absent → legacy path" — which is a documented contract, not a mask.)
8. **LOG VOLUME IS NOT IMPORTANCE.** A failing path that retries is loud by
   construction. Rank by what the user actually needs, not by line count.
9. **A FLOOR IS SAFE; AN EXPIRY IS NOT.** Size every budget/TTL from the REAL refresh
   cadence, not from a guess. A round-robin tick means per-item refresh is
   `N x period` — a flat expiry shorter than that silently kills LIVE state.
11. **PER-RUNTIME PATHS, NOT A LOWEST-COMMON-DENOMINATOR COMPROMISE (marth 2026-09-09).**
    Both binaries are unpacked now (1.6.1170 and 1.5.97), so version fragility is no
    longer a reason to weaken a mechanism. **Whatever is proven on 1.6 is what ships on
    1.6.** 1.5 gets its OWN separate path, behaviourally identical, with every offset and
    id verified against the 1.5 LAYOUT. Never degrade the proven path to make one
    construct span both runtimes, and never assume a construct valid on one layout is
    valid on the other — the layouts genuinely differ (17 vs 18 input contexts shifts
    every `ControlMap` member past `controlMap[]`, which is how MFO shipped a
    session-ending crash).
    **APMF's exposure (re-counted 2026-09-15; `native/` has ZERO `REL::ID(` sites — the
    older "six single-version `REL::ID` sites" sentence never described a `REL::ID` count).
    The six single-runtime MECHANISMS, each now placed per runtime from the CONFIRMED
    address table (`Docs/HOOK-SITE-COVERAGE.md` §1a has the per-runtime state):**
    - `core/CastClassify.cpp:60-62` (+gate in `Install()`): the +0x10/+0x18/+0x4c `CombatMagicItemData` offsets + slot-1 thunk — 1.6.1170 and 1.5.97, exact-version gate.
    - `core/AiCastSeats.cpp` GROUP C (`kWeaponClasses` + gate in `Install()`): vtables via `VTABLE_CombatInventoryItem{Melee,Ranged,Shield,Torch}` VariantIDs, slot-0x0C expected value per runtime — 1.6.1170 and 1.5.97, exact-version gate.
    - `core/CastSeats.cpp` `kAimTargetOverride = 0x30` (`CombatAimController`): confirmed identical on 1.6.1170 / 1.5.97 / 1.7.104, runs ungated by evidence.
    - `core/CastSeats.cpp` / `core/AiCastSeats.cpp` `Out16` (`GetMagicTarget` hidden sret): confirmed identical on all three, ungated by evidence.
    - `core/EquipGate.cpp` `CallSiteName()`: 0x0F call-site LABELS, 1.6.1170-only table consulted only there; cosmetic.
    - `core/NonAliasProbe.cpp` `kPutCreatedPackage = 0xDF`: confirmed same slot/function on all three, VR-refused only.
    Under this rule a new binary gets its OWN column sourced and VERIFIED (address library
    plus the unpacked binary), not a shared guess and not a construct chosen because it
    happens to span both. Gates on per-binary literals compare the EXACT version, never
    `REL::Module::IsAE()/IsSE()` (in 3.7.0 `IsSE()` is the `default:` arm — any 1.5.x/1.7.x
    reads as SE, so a family test would admit an unverified build). 1.7.104: nothing placed,
    by marth's call — and CommonLib terminates at `SKSE::Init` with the address-library
    dialog on 1.7.104; APMF's gates never run there. The REAL guard against a wrong-id
    resolve on a build that does load is the slot-0x0C expected-value compare (Group C) and
    the RTTI string compare (CastClassify): 3.7.0's `id2offset` never nulls a missing id.

10. **PROPER SOLUTIONS, NOT WORKAROUNDS.** Never work around unless absolutely
    needed; solve the root cause. If a compromise is genuinely unavoidable, FLAG it
    explicitly and record why — never bury it.

## Standing tension to resolve, not to ignore

`Docs/INVARIANTS.md #0` currently forbids a channel from calling `Actor::StartCombat`
outright ("MUST NEVER call a function that SELECTS WHAT an AI will decide to do").
Principle 1 (portals) proposes forcing combat purely as a SUBSTRATE so that engine
machinery exists, with every non-cast facet denied. These are in genuine tension. If a
design needs it, AMEND #0 explicitly and in writing (as its fourth action **(d) COMPOSE**
was added) with the deny-set that makes it safe — do NOT quietly violate it.

## REVIEW + MODEL RULES (marth 2026-09-06) — mirrored from MFO's CLAUDE.md, same rules both repos

1. **EVERY COMMIT GETS AN OPUS 5.5 DIFF REVIEW.** Not just pre-cut, not just risky ones — each commit, as it
   lands. CI-green is not a review and the coordinator's own read is not a substitute. Give the reviewer the
   BRIEF the commit was written against so it can catch unrequested scope, tell it to be adversarial, and
   have it review the BRANCH's files (`git show <branch>:<path>`), never the main working copy.
2. **WE FIX EVERYTHING THE OPUS 5.5 REVIEW FINDS.** marth, verbatim: *"A rule with fable reviews, we fix
   everything it finds."* There is no triage into blocker-vs-follow-up and no dropping a finding because the
   code that would hit it is dormant, rare, or "a design cycle". Fix them all, in severity order, before the
   branch merges or deploys — **with the ONE exception rule 9 defines: SEV-4/SEV-5 findings may be DEFERRED
   into `Docs/REVIEW-BACKLOG.md` and drained as a batch. Deferred is not dropped, and rule 9's carve-outs
   (co-save / threading / ABI, and anything the next field cycle exercises) are never deferrable.** If a finding is genuinely wrong, say WHY with evidence and get it dropped
   explicitly — do not silently downgrade it. If one truly cannot be fixed in this cycle, that is a
   STOP-and-report to marth, not a decision the worker or the coordinator makes alone.
   (Why: the 2026-09-06 deny/heal failure shipped as a reviewed, CI-green, deliberate change. Deferred
   findings are how a known defect reaches the deck wearing a review's approval.)
3. **AN OPUS AGENT WRITES THE CODE — INCLUDING SMALL CHANGES.** Not a cheap model, and NOT the coordinator
   itself. marth set the threshold LOW on purpose: *"by reasonably sized I mean smaller. but we cant afford
   the sloppy work weve been getting from teh cheap agents."* The driver is QUALITY, not token size.
   Cheap models (Sonnet/Haiku) are NOT for authoring code at all — reserve them for non-authoring mechanical
   grinds, and check their work even there. The coordinator dispatches, reads diffs, and directs corrections
   back to the worker holding the file context; hand-edits are for context-free one-liners only.

9. **REVIEW CYCLES MUST CONVERGE — SEVERITY FLOOR + A DEFERRED BACKLOG (marth 2026-09-08).** A review round
   that returns **nothing above SEV-3 ENDS the cycle.** Remaining SEV-4/SEV-5 findings are DEFERRED, never
   dropped: recorded in `Docs/REVIEW-BACKLOG.md` with the finding's VERBATIM text, its severity, the SHA it
   was raised against, and the reviewer's reasoning. Anything above SEV-3 keeps the cycle running as before.
   **Drain the backlog in ONE batch** at a natural boundary — before a release cut, or before a field cycle
   touching that subsystem — as a single agent round with a single Opus 5.5 review.
   **CARVE-OUTS that are ALWAYS fixed immediately, whatever their nominal severity:** (a) co-save layout,
   threading, or ABI / byte-shared-header findings — a SEV-4 threading finding is a SEV-4 right up until the
   day it is not, and `g_forcedWeapon`'s unguarded read was graded with "no live race today" while being the
   exact shape of the 2026-08-17 SEV-1; (b) anything the NEXT field cycle will exercise, because a deferred
   finding that corrupts the next test costs a whole deploy cycle to rediscover.
   **The backlog must be cross-referenced from MAP.md's "What breaks" entry** for each affected subsystem, so
   the next agent to touch that code reads the open findings BEFORE it writes. A deferred finding that is not
   surfaced at edit time comes back as a higher-severity one.
   (Why: the cost is per-ROUND, not per-finding. An author round plus its review costs ~250-400k tokens
   whether it fixes one finding or eight, so chasing a lone SEV-5 costs the same as draining the whole
   backlog. Measured 2026-09-08: one branch consumed six author/review rounds — ~1.8M subagent tokens across
   the session — with severities converging SEV-2 -> SEV-3 -> SEV-4/5 while every round still paid full price.
   This rule does NOT weaken rule 8 or the fix-everything rule: nothing is dropped, and a SEV-1/SEV-2 still
   blocks the merge.)
10. **SCALE REVIEW DEPTH TO THE DIFF'S SHAPE — the review still happens, its DEPTH varies (marth 2026-09-08).**
   Rule 8 stands: every commit gets a review. But match the instrument to the change.
   - **Tier 1 — doc/comment-only.** Prove it mechanically instead of dispatching: strip `//` comments from
     every touched source file on both sides of the commit and diff; they MUST be identical. Any code delta
     at all escalates to tier 2 or 3. (A comment-only commit consumed an 85k adversarial review on
     2026-09-08 to conclude what that strip-and-diff proves in seconds — which is exactly the check the
     reviewer itself ran first.)
   - **Tier 2 — localized change inside an existing mechanism.** Focused Opus 5.5 review, carrying the
     cleared-ledger of what earlier rounds already proved so it does not re-derive settled ground.
   - **Tier 3 — NO DOWNGRADE EVER.** New mechanism, threading, co-save, ABI or byte-shared headers, engine
     seats/hooks, or a TU split. Full adversarial Opus 5.5 review. A TU split is tier 3 no matter how mechanical
     it looks, because "CI-identical" is not a claim a split may assert.
   Never downgrade the REVIEWER to a cheap model at any tier. Fable found every defect that mattered this
   week; cheap review is how the 2026-09-06 regression shipped.

## LOCAL CommonLibSSE SOURCE — verify symbols here, do NOT fetch upstream

Two checkouts exist on this machine. **They are not interchangeable. Using the wrong one
re-creates the exact invented-symbol CI failures the "NEVER GUESS AN API OR A SYMBOL" rule exists
to prevent** — a symbol can be real in one tree and absent in the other.

- **`/mnt/gaming/modlists/Projects/_commonlib/pinned-3.7.0-c4ab853d/`**  ← **AUTHORITATIVE. VERIFY HERE.**
  `CharmedBaryon/CommonLibSSE-NG` @ `c4ab853d095e81e3390b282d7ba01ab2f24ebf25` = commonlibsse-ng **3.7.0**.
  **SOURCE OF TRUTH (F0, 2026-09-23): OUR MIT FORK, via OUR registry.** CI builds CommonLib from
  `marthofdoom/CommonLibSSE-NG` branch **`mit-3.7`** @ `7382a9a5bf0e2179036d7b3c56fc2f165891de71`
  (= `c4ab853d` + a README-only commit; library source byte-identical to the pinned tree), served by
  `marthofdoom/vcpkg-registry` @ `fdea6987d1cc0325003c0b654ea93e694f511e44` (port `commonlibsse-ng`
  3.7.0#1, which `native/vcpkg-configuration.json` points at instead of colorglass). Local clones:
  `_commonlib/mit-3.7-fork/` and `_commonlib/vcpkg-registry/`. Until the fork carries its own changes (F1+),
  the pinned tree IS its source. **Every symbol/vfunc/signature claim must be checked against THIS tree**
  (or the fork's `mit-3.7` tip once it diverges). If it is not here, it does not exist for our build.
- **`/mnt/gaming/modlists/Projects/_commonlib/live-alandtse-ng/`**  ← reference only, DO NOT verify against.
  `alandtse/CommonLibSSE-NG` branch `ng`, currently **v7.2.0** — the actively maintained fork (the pinned
  CharmedBaryon repo has not been pushed to since 2024-09-04). Useful for seeing how upstream solved
  something, or for planning a migration. It has bindings 3.7.0 does NOT (e.g. more `StartCombat` /
  `HasQuestObject` overloads), so verifying against it will produce code that fails CI.

Refresh with `git -C <dir> fetch --depth 1` if ever needed; the pinned tree must stay at that exact SHA.
**Still verify against the DISASSEMBLED binary, not the header, for anything ABI-shaped** — CommonLib
declarations are not ABI-trustworthy (a wrong `GetMagicTarget` signature with a hidden sret out-slot once
made a "passive" probe crash the game).

**DECIDED (marth 2026-09-23): no migration to the alandtse fork** (7.x is GPL-3.0-or-later, our mods are MIT).
We own an MIT 3.7.0 line instead: plan in `_commonlib/fork-plan-2026-09-23/own-addrlib-design.md`
(F0 fork + registry, byte-identical; F1 corrections + self-check; F2 1.7.104 clean-room). Never copy code
from `live-alandtse-ng/` into the fork. Changing the ABI source under a plugin doing vtable and offset work
breaks in the FIELD, not in CI, so each fork stage is its own scoped brief with its own Opus 5.5 review.

## FIELD DIAGNOSIS + AGENT REUSE (marth 2026-09-06)

**4. WHEN AN IN-GAME TEST DOES NOT DO WHAT WE EXPECT, IT GOES STRAIGHT TO OPUS 5.5.** marth: *"when a test in
game doesnt do what we expect, immediately goes to fable."* The split is strict:
- **The coordinator GATHERS.** Pull the deck logs; verify the deployed DLL sha against the branch that
  produced them; check the INI switch states; build a consolidated EVIDENCE file (tag histograms, message
  shapes, per-ACTOR and per-HAND attribution, timestamps). This works and makes Opus 5.5 fast.
- **OPUS 5.5 CONCLUDES.** Do NOT arrive with a root-cause theory. Hand over the evidence and the brief.
Measured 2026-09-06: from one 8-minute deck log the coordinator produced FOUR confident root causes and
Fable overturned ALL FOUR using that same evidence file (a "deny hole" that was by-design chaining; a spell
attributed to the follower that four Chaurus Reapers were casting; a "stale" proxy that was actually read
before it was minted; a config problem that was a code problem). Also pass Opus 5.5 any METHOD constraint marth
has already given — e.g. do NOT argue from loot arrival counts or travel/arrival ratios; a follower walking
past loot during an ordinary follow produces arrival-shaped lines that prove nothing.

**5. REVIEW FINDINGS GO BACK TO THE AGENT THAT WROTE THE PATCH, via SendMessage — never a fresh agent.**
marth: *"If the same patch has more issues it would make sense to reuse the same agent for the iterations."*
The author already holds the file context, the symbol verifications and the reasoning; a fresh agent
re-orients from zero over the same files (measured cost of getting this wrong: 173k / 224k / 108k subagent
tokens re-deriving what the author already knew). This IS the sanctioned resume case under the anti-drip-feed
rule above — applying review findings is "fixing a real error", not adding scope. A resumed agent does NOT
re-read its original brief, so the message must still carry the hard rules (verify symbols, CI-green on your
own SHA, no merge/tag/force-push, the file boundary). Spawn a NEW agent only for a genuinely different patch,
a different repo/tree, or work the author was never briefed on — and never two agents in one build tree.
