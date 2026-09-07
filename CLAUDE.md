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

1. **EVERY COMMIT GETS A FABLE DIFF REVIEW.** Not just pre-cut, not just risky ones — each commit, as it
   lands. CI-green is not a review and the coordinator's own read is not a substitute. Give the reviewer the
   BRIEF the commit was written against so it can catch unrequested scope, tell it to be adversarial, and
   have it review the BRANCH's files (`git show <branch>:<path>`), never the main working copy.
2. **WE FIX EVERYTHING THE FABLE REVIEW FINDS.** marth, verbatim: *"A rule with fable reviews, we fix
   everything it finds."* There is no triage into blocker-vs-follow-up and no deferring a finding because
   the code that would hit it is dormant, rare, or "a design cycle". Fix them all, in severity order, before
   the branch merges or deploys. If a finding is genuinely wrong, say WHY with evidence and get it dropped
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

## LOCAL CommonLibSSE SOURCE — verify symbols here, do NOT fetch upstream

Two checkouts exist on this machine. **They are not interchangeable. Using the wrong one
re-creates the exact invented-symbol CI failures the "NEVER GUESS AN API OR A SYMBOL" rule exists
to prevent** — a symbol can be real in one tree and absent in the other.

- **`/mnt/gaming/modlists/Projects/_commonlib/pinned-3.7.0-c4ab853d/`**  ← **AUTHORITATIVE. VERIFY HERE.**
  `CharmedBaryon/CommonLibSSE-NG` @ `c4ab853d095e81e3390b282d7ba01ab2f24ebf25` = commonlibsse-ng **3.7.0**,
  exactly what the colorglass vcpkg registry pins (`native/vcpkg-configuration.json` baseline
  `6309841a…`) and therefore exactly what CI compiles and links. **Every symbol/vfunc/signature claim
  must be checked against THIS tree.** If it is not here, it does not exist for our build.
- **`/mnt/gaming/modlists/Projects/_commonlib/live-alandtse-ng/`**  ← reference only, DO NOT verify against.
  `alandtse/CommonLibSSE-NG` branch `ng`, currently **v7.2.0** — the actively maintained fork (the pinned
  CharmedBaryon repo has not been pushed to since 2024-09-04). Useful for seeing how upstream solved
  something, or for planning a migration. It has bindings 3.7.0 does NOT (e.g. more `StartCombat` /
  `HasQuestObject` overloads), so verifying against it will produce code that fails CI.

Refresh with `git -C <dir> fetch --depth 1` if ever needed; the pinned tree must stay at that exact SHA.
**Still verify against the DISASSEMBLED binary, not the header, for anything ABI-shaped** — CommonLib
declarations are not ABI-trustworthy (a wrong `GetMagicTarget` signature with a hidden sret out-slot once
made a "passive" probe crash the game).

**Open strategic question, not yet decided:** we are pinned four major versions behind (3.7.0 vs 7.2.0) on
a dormant repo. Migrating to the alandtse fork is its own scoped brief with its own Fable review — changing
the ABI source under a plugin doing vtable and offset work breaks in the FIELD, not in CI. Do not start it
as a side effect of another task.

## FIELD DIAGNOSIS + AGENT REUSE (marth 2026-09-06)

**4. WHEN AN IN-GAME TEST DOES NOT DO WHAT WE EXPECT, IT GOES STRAIGHT TO FABLE.** marth: *"when a test in
game doesnt do what we expect, immediately goes to fable."* The split is strict:
- **The coordinator GATHERS.** Pull the deck logs; verify the deployed DLL sha against the branch that
  produced them; check the INI switch states; build a consolidated EVIDENCE file (tag histograms, message
  shapes, per-ACTOR and per-HAND attribution, timestamps). This works and makes Fable fast.
- **FABLE CONCLUDES.** Do NOT arrive with a root-cause theory. Hand over the evidence and the brief.
Measured 2026-09-06: from one 8-minute deck log the coordinator produced FOUR confident root causes and
Fable overturned ALL FOUR using that same evidence file (a "deny hole" that was by-design chaining; a spell
attributed to the follower that four Chaurus Reapers were casting; a "stale" proxy that was actually read
before it was minted; a config problem that was a code problem). Also pass Fable any METHOD constraint marth
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
