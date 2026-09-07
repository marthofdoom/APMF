# CLAUDE.md — APMF / "Harbinger" (AI Package Management Framework), SKSE C++ plugin

APMF is a per-facet AI **moderator** for Skyrim NPCs: client mods declare intent,
APMF arbitrates who owns each facet, denies the losing sources, and composes the
result onto the actor — WITHOUT substituting the actor's package. MFO
(marth's Follower Overhaul, a separate repo) is its primary client and showpiece.

**Read first:** `design.md` (the model), `Docs/ARCHITECTURE.md`, `Docs/INVARIANTS.md`
(24 numbered rules — code cites them as `#N`), `Docs/CHANNEL-MAP.md` (channel ↔ hook
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
