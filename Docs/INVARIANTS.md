# APMF Invariants

Numbered rules that keep APMF correct, version-robust, and crash-free. MAP.md and
the code cite these as `#N`. Break one and you get a data race, a CTD on a game
update, a mislabeled gate, or an actor left in a mutated state. Read the relevant
ones before touching a subsystem.

## Design principles

**#0 — APMF MODERATES; it MUST NOT MANUFACTURE OR SUSTAIN AN AI DECISION (the
hardest rule, the one a CTD taught us).** An APMF channel may do exactly three
things: (a) ARBITRATE — record which client owns a facet; (b) DENY — suppress the
losing source at its source (AV writes, movement full-block, detection AVs, package
yield, pausing an actor's own in-progress dialogue); and (c) PROMOTE A BOUNDED,
ONE-SHOT, CLIENT-REQUESTED ACTION for a facet that has no meaningful "deny" form and
involves no AI decision to arbitrate around — a single anim-graph event, a sticky
draw/sheathe, a stance toggle, the client's named idle — fired ONCE at Engage/Release, on the game thread,
with no per-tick `Tick` work and no re-assert loop. (ch.12 idle v2, ABI v17, 2026-09-25: ONE
`AIProcess::PlayIdle(actor, idle, target)` at Engage for the TESIdleForm and the reference the
client declared in `param.form` / `param.target`, one more per Repoint / owner change because each
is a new declaration, and at Release ONE `IdleForceDefaultState` ONLY when that idle is still HELD —
the actor's graph has not raised `IdleStop` since the call. A one-shot idle that ended by itself
gets nothing at Release. "Held" is INFERRED from the graph event `IdleStop` and must be OBSERVED in
the first field log (a one-shot idle's Release line lists IdleStop in its tag list). The reset is
NEVER sent while the actor's sit/sleep state is not kNormal (furniture). A form-free owner change
over a held v2 idle gets the same guarded reset and drops the v2 entry. The claim ends on a refused or unplayable idle and on the owner's death;
it is never re-played by Harbinger. Still (c): the idle is the client's, not an AI selection.) A channel MUST NEVER call a
function that SELECTS WHAT an AI will decide to do when the client already has its
own proven mechanism for making that selection — NOT `Actor::StartCombat`, NOT
`MagicCaster::CastSpellImmediate`, NOT a movement drive-feed, NOT a direct
`selectedSpells`/`EquipShout`-style write that picks a target/spell/power for the AI
to later act on. That class of DECISION-SELECTION is BEHAVIOR, and behavior belongs
to the CLIENT (it executes with its own proven mechanisms; APMF only denies
competitors, or arbitrates the claim, so the client's behavior reaches the actor —
design.md §1a). **The bright line:** a lawful promote is a single deterministic call
whose outcome does not stand in for an AI decision (idle-play, weapon-draw,
stance-toggle); a forbidden generate calls a function that picks WHAT the AI decides
(a target, a spell, a shout) or drives it continuously. (The one `CastSpellImmediate` APMF
makes is action (e) below, ADOPTED 2026-09-23 by marth: from an APMF-owned MARKER, never from
the actor, for a spell and a point the client declared, and under condition (8) it is not an
endpoint. The one `StartCombat` APMF makes is action (g) below, ADOPTED 2026-09-25 by marth: once per
client declaration, against the target the client named, through the engine's own entry function. Action (h) below
denies that same entry function's answer for a declared actor and window; it calls nothing.) **Cautionary case:** ch.6
combat-target once called `StartCombat` (to command a target) — wrong LAYER, and
with a bad reloc signature it was a hard AV (EXCEPTION_ACCESS_VIOLATION inside
StartCombat). The deny-only rule made that whole crash class structurally
impossible: APMF made no such call at all. (Since (g), 2026-09-25, it makes exactly one kind: the
three-argument form verified on both images, from ch.21 only, never from ch.6.) ch.6 is arbitration-only and the client
commands the target. **ch.8 is ARBITRATION + DENY, not arbitration-only** (corrected 2026-09-07): a `kIntent_SelectSpell` claim is enforced at two gates — `core/CastGate.cpp:124` (0x0A `CheckCast`) and `core/EquipGate.cpp` (0x0F `CheckShouldEquip`) deny any spell/staff that is not the claim's `param.form` (plus its allow-list). The "arbitration-only" wording dates from the 2026-09-02 #0 correction and was never updated when the deny landed the same day. What ch.8 does NOT do is WRITE the selection —
it denies everything else, and the client writes its own `selectedSpells`. That is the
part #0 is about. **ch.14 shout-power's direct
`EquipShout` call was the same anti-pattern in miniature** — no crash (the function
is bound and safe), but it duplicated the "APMF selects, not the client" mistake
this rule exists to end; it is now converted to arbitration-only, matching ch.6 (and ch.8's
"APMF never writes the selection" half).

**The fourth legal action, added 2026-09-05: (d) COMPOSE — answer the ENGINE'S OWN
decision seats so the engine's own logic produces what the claim asked for.** This
is NOT a loophole in the rule above; it is the rule taken seriously. A composed
seat makes NO call that performs the behavior (`core/CastSeats.cpp` makes no
`EquipSpell`, no `CastSpell`, no `CastSpellImmediate`, no `NotifyAnimationGraph`,
no caster-state write) — it changes what a vfunc SEES and lets the AI decide. The
cast facet's retired forced drive, which DID call `CastSpellImmediate`, is exactly
what this replaced. #20 states the mechanism, the one permitted non-chaining
answer and its three conditions; read it before adding a composed seat anywhere.

**ADOPTED 2026-09-23 by marth. The fifth legal action (ABI v11, `core/PositionCast.cpp`): (e)
DELIVER AT A POINT — one client-declared remote cast from an APMF-owned marker.** Approved
with a standing condition, verbatim: *"this is not an acceptable endpoint, proper animations
are required for ALL actions. So choosing a path that makes that impossible is incorrect."*
That is condition (8) below, and it is why the code still ships it OFF: `[PositionCast]
bPositionCast` defaults to 0 in both `APMF.ini` and the code, and every request is refused by
name until a user sets it to 1. A
`kIntent_Cast` RequestEx with `kCastFlag_AtPosition` makes APMF place an XMarker at
`param.pos` and call `CastSpellImmediate` on the MARKER's instant caster, blamed on the
actor. This is written here, in the rule it touches, rather than done quietly (CLAUDE.md
"standing tension"). It is legal only while ALL of these hold:
1. **There is nothing to compose.** No engine seat can aim a location spell at a point
   for an NPC: the cast core's Target Location arm never reads the target it is handed
   and lands at the caster's own magic node, and seat 0x0A carries an `Actor*`
   (`Docs/ADDRESS-TABLE-2026-09-15.md`, ADDENDUM 2026-09-23, both runtimes). Composition
   is not a weaker answer here, it is no answer.
2. **APMF selects nothing.** The client declared the spell and the point. APMF picks no
   spell, no target and no point. (CORRECTED 2026-09-23: an earlier wording said "no NPC AI
   decides to put a spell at a remote point". That is wrong: the field shows a follower's own
   AI choosing and animating runes (Natura Stone Rune FE209C83 fired 5 and 25 times). The true
   statement is narrower: no engine seat lets an NPC's cast land a Target Location PROJECTILE
   at a point, because the cast core places that projectile only from the player's crosshair
   pick.)
   **THE LIMIT THIS PUTS ON (e) ITSELF.** The same fact binds the marker: its caster has no
   out-actor and no pick, so the cast core never launches a Target Location projectile from it
   (AE inline `0x5bd04e`-`0x5bd08e` + TestProjectilePlacement 34453 `0x5c02e0`; SE 33671
   `0x550550` +0x8d; the pick is filled only by the player branch, AE 34457 `0x5c0470`, gated at
   AE `0x5bc3ec` / SE `0x54cf8a`). So (e) delivers only Target Location effects WITHOUT a
   projectile. A rune, a trap or any spell whose effect carries a projectile is REFUSED by name
   on the game thread (it would silently place nothing).
3. **One call, once.** On the main thread, once per request, the engine's own
   sequence (Papyrus `Spell.RemoteCast`: `InterruptCast(false)` then
   `CastSpellImmediate(spell, false, none, 1.0, false, 0.0, blame)`). No Tick, no
   re-assert, no retry. A refusal is logged and the request is dropped.
4. **The actor is not touched.** The caster is the marker, so the actor does not
   animate and its hands, its casters and its cast facet are unchanged. Nothing is
   switched on in the actor, so principle 2 has no facet to deny.
5. **It is never a claim.** It never enters the control map, so no seat can read it as
   an actor target, and it holds no facet.
6. **Scope is closed by name.** Target Location, fire-and-forget, no summon, not
   disease/ability/addiction, exactly 1.6.1170 or 1.5.97, not VR. Everything else is
   refused in the log. Widening it (another delivery, a held stream, casting from the
   actor) is a new amendment, not a code change.
7. **The actor's cast facet keeps its owner.** A position cast is refused by name,
   synchronously, while a LIVE `kIntent_Cast` claim on that actor would outrank a driving
   request at the position cast's basis: a higher basis (a `kCastFlag_DenyHandOnly` floor
   included), or an equal basis that drives something (earliest keeps a tie; an equal-basis
   deny-only floor loses to a driving request, as everywhere else in the cast channel).
   APMF has no client identity, so "another client's claim" is decided the way every facet
   is: by basis (`ControlMap::CastFacetOutranks`, `core/ControlMap.cpp`). Only PUBLISHED
   claims are seen; one still in the request queue is not. It runs at the call and AGAIN on
   the main thread just before the cast (a claim may publish in between); a non-finite basis
   is refused first. **Consequence, stated because there is no client identity:** a caller's
   OWN driving cast claim at the same (or a higher) basis blocks its own position cast.
8. **Not an endpoint.** A marker cast does not animate the actor, and marth requires proper
   animations for ALL actions. No client may ship a user-facing action on (e) alone. An
   animated path (the actor's own AI cast, aimed at the point, composed through the engine's
   seats) must be researched and preferred; (e) is only a stepping stone or the delivery half
   of an animated composition.
The marker itself inherits #19's spirit: it is a runtime-created 0xFF reference, deleted
one frame after the cast by its tracked handle, forgotten (never touched) at a world
boundary, and it is recorded in the co-saved marker ledger (#15, record `'XMRK'`) so the
load of a save that captured it deletes it.

**ADOPTED 2026-09-25 by marth ("yes, start the target pin", ClickUp 86e3cr9u7; design corrected the
same day: "APMF offers pinning, it must block engine pinning"). The sixth legal action (ABI v13,
`channels/TargetPin.cpp`): (f) PIN A DECLARED TARGET — deny the engine's own target selection AT ITS
SOURCE, among the targets the engine itself holds and can locate.** This puts a client-named actor where the engine's
target pick would go, which the rule above names as forbidden; it is written here, in the rule it
touches, rather than done quietly (CLAUDE.md "standing tension"). The client declares {actor, target}
with `kIntent_TargetPin`. The engine picks an actor's combat target in
`CombatController::UpdateTarget` (called once per `UpdateCombat`), which asks each active
`CombatTargetSelector` through vtable slot 6 (`SelectTarget`) and hands the first answer to
`SetTarget`. APMF's seat on slot 6 of both selector classes (Standard and Fixed) chains the original
and replaces its answer with the declared target. It is legal only while ALL of these hold:
1. **APMF selects nothing.** The client named the target. APMF picks no target, no foe order and
   no fallback; an inert claim (the target is not an actor, 0, or the actor itself) answers nothing.
2. **APMF only chooses among targets the engine itself holds AND can locate.** The answer is
   replaced only when the engine's own selector answered with a target (the actor is fighting) AND
   the declared target is in the actor's `combatGroup->targets` (read under the group's own lock)
   AND that entry is not flagged `kTargetLost` (`CombatTarget::flags`, u16 +0xA6, bit 1). A declared target the
   engine does not hold as a combat target is NOT written: counted and logged. So the pin never starts
   a fight, never revives one the engine ended, never overrides the engine's judgement that there is no
   foe, and never makes a non-combatant a target on its own. A non-foe becomes pinnable only once
   something else (the client's own combat entry, a separate facet) makes it a combat target. Combat
   ENTRY is not this action: it is (g) below, its own intent (`kIntent_CombatEntry`), and the pin
   itself still never enters combat.
3. **It is the target facet and nothing else.** No attack selection, cast, equip, movement,
   aggression or perception write. Everything the NPC does against the target is its own AI.
4. **It is a SOURCE deny, chained.** The selector's original always runs (#17) and keeps its own
   bookkeeping; only its OUTPUT is replaced, before `UpdateTarget` compares it with the controller's
   target and calls `SetTarget`. From the first combat update on, the engine's pick therefore never reaches
   `targetHandle`, `currentCombatTarget`, or anything the rest of that update reads. (A new
   controller's very first target is set before any update asks the selectors; the pin applies
   from that first update.) The engine CLEARING the target
   (`SetTarget(0)`) is not denied (condition 2). There is NO after-the-fact rewrite: the
   `UpdateCombat` hook ch.20 also installs is observe-only and exists to log a source-seat MISS or an
   OVERWRITE loudly (principle 7), never to paper over one.
5. **It ends by itself.** marth, 2026-09-25: *"If APMF can no longer track the target, it's lost,
   and dropped."* Target LOST, dead, disabled, not loaded or unresolvable, or the owner dead: APMF
   RELEASES the claim itself (`targetpin::Poll`, the ch.19 monitor seat), logs `pin ended:
   <reason>`, and repeats the reason on the Release line. This is the only case where APMF releases
   a client's pin claim. The pin merely PAUSES (the engine's selection stands, the claim stays) while
   the engine holds no target or the declared target is not yet in the group's targets, since the
   client's combat entry may still add it. Released, outranked or dropped (unload, save load,
   revert): the engine's own selection answers again. Release restores nothing (#5a).
6. **Scope is closed by name.** Exactly 1.6.1170 or 1.5.97, not VR, the two selector vtables and
   the Character vtable only (never the player), `[TargetPin] bTargetPin`, the address self-check on
   all three, and a per-call vtable-identity check before the raw selector offset (+0x10) is read
   (#20's raw-offset guards). Widening it (a combat entry, a target list, a priority order) is a new
   amendment, not a code change.
7. **The world's reaction belongs to the client** (CLAUDE.md principle 2 scope, marth
   2026-09-25). Crime, bounty, faction and aggression reactions to the declared fight happen as
   the engine does them, and are not undone.

**ADOPTED 2026-09-25 by marth (ClickUp 86e3940zb: "a third-party modder needs to make an NPC fight a
target"). The seventh legal action (ABI v14, `channels/CombatEntry.cpp`): (g) ENTER COMBAT AGAINST A
DECLARED TARGET -- one call of the engine's own combat entry.** The rule above names `Actor::StartCombat`
as forbidden; this is written here, in the rule it touches, rather than done quietly (CLAUDE.md "standing
tension"). The client declares {actor, target} with `kIntent_CombatEntry`. Harbinger calls
`Actor::StartCombat(target, nullptr)` (1.6.1170 id 38561 `0x6B6930`, 1.5.97 id 37608 `0x6251B0`; three
arguments, verified register by register on both images -- the old ch.6 CTD was a two-argument call that
left garbage in R8). Not in combat, the engine builds the controller and group and makes the target the
group's first combat target, or builds nothing (its `CombatGroup::AddTarget` failing destroys the new
controller); already in combat, it adds the target to the group's targets and keeps its own current
target. It is legal only while ALL of these hold:
1. **The client names the target; APMF selects nothing.** No target, foe order, fallback or "nearest
   enemy" is chosen by APMF. A claim whose target is 0, the actor itself, or not an Actor makes no call
   (refused at the call, or ended one frame later, logged). The player may be the target and may never be
   the actor.
2. **The engine's own entry path, and nothing else.** The one function the engine itself calls to start
   a fight (its callers include AE 40814, which passes the same nullptr third argument). APMF writes no
   controller, group, target, detection, aggression or faction state, builds no selector, and calls no
   `SetTarget`. Whether and how the actor enters is the engine's: it refuses a restrained, unconscious or
   dead actor, a dead target, a target failing its own distance test, an actor that is one particular
   engine-global actor (AE id 401069 / SE id 514905, an identity test), and a few engine flags. A refusal
   is not retried: it ENDS the claim (condition 5).
3. **One call per declaration, which is not sustaining.** On the main thread, one frame after the claim
   is published (re-validated then), exactly one call per Engage and one per Repoint / owner change.
   Each call answers a declaration the client just made (a Repoint is a new declaration; so is another
   client's claim becoming the winner). No Tick, no re-assert, no retry, no re-entry when the engine
   ends the fight: re-entering on Harbinger's own initiative would SUSTAIN a decision, which the rule
   above forbids. After the claim has ENDED, another entry is a NEW request from the client. **The
   engine giving up is final for that target (marth: "the engine gave up = dropped"):** when a claim
   ended because the engine ended the fight (`combat ended`) or refused the entry, a rival claim that
   then takes over the actor's facet naming the SAME target makes NO call -- it is ended with the same
   reason. That record is kept per actor and per target until the actor's last combat-entry claim is
   released. A rival naming a DIFFERENT target is a different declaration and gets its one call.
4. **Release stops forcing and undoes nothing.** Release calls no `StopCombat` and restores nothing
   (#5a): once entered, the fight is engine state -- controller, group, members told, detection, crime --
   and stopping it would be an undo that also tears down combat the actor may have for its own reasons.
   The engine ends it as it ends any fight; a client that wants it over calls `StopCombat` itself.
5. **It ends by itself, and is never left live and inert** (marth's ch.20 ruling: "if APMF can no
   longer track the target, it's lost and dropped"). APMF releases the claim and logs `entry ended:
   <reason>` (repeated on the Release line) when: the ENGINE REFUSED the entry (`engine refused entry:
   <cause if known>`); the entry could not be attempted (the target is not an Actor; the actor is not
   loaded, has no AI process or is dead; the target is not loaded, disabled or dead); the FIGHT ENDED
   (the entry succeeded and the actor's `combatController` POINTER is now null -- `combat ended`; the
   pointer is read, the controller is never dereferenced, which is why `Actor::IsInCombat`, which reads
   a byte at controller +0x43, is not used); or the target is dead, disabled, not loaded or unresolvable,
   or the owner dead (`combatentry::Poll`, the ch.19 / ch.20 monitor seat). Endings are enqueued from
   the main-thread pump or Poll, never from inside Drain. Released, outranked or dropped (unload, save
   load, revert): nothing is saved and nothing is undone.
6. **It is the entry facet and nothing else.** It does not choose whom the actor fights first (the
   engine's selector does; the client claims (f) `kIntent_TargetPin` for that), and it claims no attack
   selection, cast, equip, movement or perception. The re-arm equip `StartCombat` performs goes through
   ch.17's seat like every other engine equip.
7. **Scope is closed by name.** Exactly 1.6.1170 or 1.5.97, not VR, a non-player actor with an AI
   process (`StartCombat` dereferences it unchecked) that is loaded and alive, a loaded, enabled, living
   target, `[CombatEntry] bCombatEntry`, and the address self-check on `StartCombat`. Joining a named
   CombatGroup (the third argument), entering against a list, re-entry on a timer, or ending combat
   (`StopCombat`) are each a new amendment, not a code change.
8. **The world's reaction belongs to the client** (principle 2 scope, marth 2026-09-25): "if a user uses
   it to cause chaos, chaos ensues." Crime, bounty, faction and aggression changes, allies and guards
   joining, combat music -- all happen as the engine does them and are not denied or undone.

**ADDED 2026-09-25 (ClickUp 86e3ex5v9, batch L, from MFO's confidence / leash assessment: a retreating
follower is pulled back into combat every tick, so the client re-issues `StopCombat` every tick). The
eighth legal action (ABI v15, `channels/CombatReentryDeny.cpp`): (h) DENY COMBAT RE-ENTRY FOR A BOUNDED
WINDOW -- refuse the engine's own combat entry for a declared actor at the entry function's own
self-check.** It turns the engine's own "enter combat" into the engine's own "refuse", which is a DENY of
an engine decision; it is written here, beside (f) and (g), rather than done quietly. The client declares
{actor, window seconds} with `kIntent_CombatReentryDeny` (`param.fval`; 0 = 10 s, clamped to 120 s).
**Where entry happens.** An actor is in combat exactly when it has a `CombatController`. Outside save
loading, a controller is built only by CombatManager AE 46873 / 46874 (SE 45573 / 45574), whose only
callers are inside `Actor::StartCombat` (AE id 38561 `0x6B6930`, SE id 37608 `0x6251B0`); the controller
constructor's one other caller (AE 37650 / SE 36642) is the save-load rebuild. So every entry -- detection,
being attacked, an ally's fight, a group join, a script, another mod -- is `StartCombat` with that actor as
`this`. `StartCombat` is not virtual and has ten callers per runtime, so neither its entry nor its callers
may be patched (#17). **The seat.** Its fourth refusal check is a VIRTUAL call on the actor itself,
`IsDead(true)` through Character vtable slot 0x99 (AE `0x6B69C4`, return `0x6B69CA`; SE `0x625242`, return
`0x625248`), made before its global spinlock, its re-arm equip and everything else it does; a true answer
jumps straight to its refusal epilogue. APMF's `write_vfunc` on slot 0x99 chains the original and changes its
answer only at that one return address. It is legal only while ALL of these hold:
1. **A deny, never an entry, never a stop.** The only change is StartCombat's own YES to its own NO, through
   a check it already makes, before it has done anything. While the window runs EVERY StartCombat for the
   actor is refused (closing-round ruling, option (a)): a new entry, and the in-combat path that adds a
   target to a fight already running. The seat reads no actor state beyond the FormID (in particular not the
   controller pointer, so no race with a `StopCombat` on another thread). APMF calls no engine function,
   writes no engine state, and never takes an actor out of a fight it is in: that fight goes on, it only
   gains no targets through StartCombat. Ending it is the client's call (`StopCombat`, once).
2. **Scoped to one call site.** Every other `IsDead` caller -- thousands per frame -- gets the original
   answer; the return-address compare is the thunk's first act after the original. The site is a verified
   call-site row (`ReentryDeny.StartCombat.SelfIsDeadCall`: StartCombat's signature, then the eleven bytes
   `B2 01 48 8B CF FF 90 C8 04 00 00` at +0x8F AE / +0x8D SE, byte-checked again at runtime by
   `REL::SelfCheck`). The TARGET's own `IsDead(false)` check inside StartCombat (a different return address)
   is never answered: another actor's entry against this one is that actor's facet.
3. **Bounded, and it ends by itself.** The window runs from the claim's OWN request time (a Repoint of that
   claim restarts it): ControlMap records it per handle at `EnqueueRequest` / `EnqueueRepoint`, and after the
   Drain that applied the claim publishes, `Settle` sets the deadline from the WINNING handle's time. A claim
   that takes over from a released rival therefore gets only what is left of its own window, and one whose
   window is already over ends at once (the ch.21 "no stale declaration" line). Until `Settle` runs -- one
   main-thread pump -- the deadline is provisional (apply time + window). When the window elapses or the
   owner dies, `reentrydeny::Poll` (the ch.19 / ch.20 / ch.21 monitor seat) releases the claim and
   logs `deny ended: <reason>`. Released, outranked or dropped (unload, save load, revert): the engine's
   entries pass again. Nothing is saved; nothing is undone (nothing was written).
4. **A client's declared entry passes (precedence).** ch.21's one `StartCombat` for the actor runs inside
   a thread-local `ClientEntryScope` and is let through, whoever's ch.21 claim it is: both are declared
   decisions, and the deny is against the ENGINE drawing the actor in. It does not end the deny claim.
5. **Observed before relied on (principle 5), and a miss is loud (principle 7).** The seat logs its first
   StartCombat self-check of the session (`seat OBSERVED`) whether or not a claim exists, and
   `reentrydeny::Poll` logs `DENY MISSED` when a denied actor gains a controller under a live window
   without a ch.21 entry passing (sampled every 250 ms, so a StopCombat and a new entry inside one sample
   are not seen as a transition -- a limit of the detector, not of the deny). The remaining known miss
   path is a DLL that wraps slot 0x99 AFTER APMF with a thunk that CALLS (rather than tail-jumps to) the
   previous entry, which hides StartCombat's return address; every Engage / Repoint line names the slot's
   owner and warns for that claim when it is not APMF's thunk (the claim is not refused). No retry, no
   fallback, no re-assert.
6. **Scope is closed by name.** Exactly 1.6.1170 or 1.5.97, not VR, the Character vtable only (never the
   player), `[CombatReentryDeny] bCombatReentryDeny`, the address self-check on the Character vtable and the
   call-site row. Denying other actors' entries against this one, ending combat, or passing any caller
   other than ch.21's `ClientEntryScope` are each a new amendment, not a code change.
7. **The world's reaction belongs to the client** (principle 2 scope). Enemies may keep attacking an actor
   that cannot fight back; that is their facet and the client's declared consequence.

**#20 — COMPOSED ANSWERS: APMF may answer the ENGINE'S OWN DECISION SEATS so the
AI decides what a claim asks for — and exactly ONE of those answers may skip the
chain.** (feat/ai-cast-seats-impl, 2026-09-05. This is the rule that lets ch.8b
work at all, and the rule that keeps it from becoming #0's forbidden "generate".)

**The mechanism.** A decision the engine makes is a PIPELINE of vfunc seats, each
reading inputs and producing an answer. APMF may sit in that pipeline and change
what a seat SEES, so the engine's own logic — its own thresholds, its own
animation, its own resource and LOS checks, its own interrupt handling — produces
the outcome the claim asked for. That is COMPOSITION, and it is categorically
different from generating behavior: no `EquipSpell`, no `CastSpell`, no
`CastSpellImmediate`, no anim-graph event, no state poke. The cast facet is the
worked example (`core/CastSeats.cpp` + `core/EquipGate.cpp`): five seats, and the
NPC's own AI performs a real animated cast at the claimed target.

**The exception, and its three conditions.** A seat is normally answered by
CHAINING (call the original first, then only ever flip its YES to NO — #17). A
seat may be answered FROM THE CLAIM WITHOUT CHAINING only when ALL THREE hold:
1. **The original is structurally un-redirectable.** Its inputs are not reachable
   through any interposable seat — it reads them directly out of an engine struct.
   (`CheckShouldEquip`'s Restore override calls the static `0x81f7c0`, which takes
   its target straight off the `CombatController` and has no caster object, hence
   no `GetMagicTarget` to redirect. Chaining is not a weaker answer there; it is
   NO answer, and it also short-circuits every downstream seat.)
2. **The forced answer is an ELIGIBILITY signal, not the act.** It says "this is
   allowed to be considered," never "do this." The engine still scores it, still
   applies its own slot/range/resource/blackboard gates, still runs its own leaf
   with its own animation, and is still free to choose otherwise.
3. **It is scoped to the exact {claim, actor, form, hand}** and to the specific
   vtables whose original IS the obstacle — verified at install, re-verified per
   call. Everything else on that same slot keeps chaining unconditionally.
If any condition fails, chain. A non-chaining answer that fails (2) is #0's
forbidden generate wearing a hook's clothing.

**Scope is the safety argument, not an afterthought.** Engine vfunc
implementations are widely SHARED: `GetMagicTarget`'s implementation is the base,
used by 13 of the 14 combat-caster vtables. An unscoped redirect there would aim
Stagger/Disarm/Offensive effects at the ally a heal claim named. So a composed
seat installs on the NARROWEST vtable set that carries the behavior — today the
Restore AND Offensive caster vtables, 2 of the 14 (`core/CastSeats.cpp:483-500`;
widened from Restore-alone in v0.9.2, because a claimed HOSTILE spell classifies
into the Offensive caster and a claim on it was otherwise inert) — AND re-tests the
claim per call. Two independent gates, either of which alone would suffice.
NARROWEST means narrowest that carries the behavior the claim needs, never
"whichever vtable we started on": widening is legitimate when a claim provably
cannot be served without it, and the per-call re-test is what makes each widening
safe. It is not a licence to install on the base.

**RELEASE ORDERING: the cleared claim is PUBLISHED BEFORE ANYTHING ELSE the
release does.** A composed seat runs on the combat thread and reads the RCU
snapshot; the instant the claim is gone from the published generation, every seat
chains again. So nothing a release does may become visible to those seats BEFORE
that publish. Concretely: `channel->Release` runs inside `ControlMap::Drain`,
while the removal is still only in the writer's PRIVATE working copy — so ch.8b's
proxy teardown (`castproxy::Free`, which un-teaches a form the seats may still be
naming) is deferred exactly one main-thread hop through `apmf::mainthread::Post`,
which `Arbiter::OncePerFrame` pumps immediately AFTER `Drain()` returns, i.e.
strictly after `Publish()`. Any future release-time engine write inherits this:
publish first, mutate second.

**A raw struct offset is permitted in a composed seat ONLY with a runtime identity
check.** #7 says never hand-write an offset. The aim-target override
(`CombatAimController+0x30`) is the one place ch.8b must, because the pinned
CommonLib has no `CombatProjectileAimController` class to reach through. It is
allowed because it carries THREE guards instead of the static_assert it cannot
have: its own INI kill-switch, an install-time RTTI derivation check, and a
per-call check that the object's vtable pointer EXACTLY equals the resolved
`VTABLE_CombatProjectileAimController`. Anything else is left untouched. Do not
copy the offset without copying all three guards.

**#1 — APMF is the gatekeeper: BLOCK the foreign input, do not force the output.**
Once APMF owns a channel on an actor, nothing else reaches that facet except through
APMF. A channel's job is to block the competing input at its source — deny the
losing source — so nothing competes. It does NOT let a source produce a write and
then override it every frame, and it does NOT itself write the behavior. A per-tick
re-assert loop is a FAILED block, a symptom of not-blocking, NOT an acceptable
pattern (design.md §1a; marth 2026-09-02). `Channel::Tick` is empty by default for
exactly this reason — a real block does no per-tick work.

**#2 — A channel that still needs re-assert is a KNOWN-INCOMPLETE block; label it
so.** Where we have not yet blocked the AI's own write to a facet, a re-assert
stopgap is permitted BUT the channel must (a) override `Tick`, (b) say
"known-incomplete block, not a clean gate" in its module header, and (c) accept
that it can LOSE to an aggressive or package-locked source. The FIX is to block the
AI's write (gate the relevant part of the AI decision layer at the 0xAD hook for
the owned channel — skip/neutralize the write), NOT to keep overriding after the
fact. Deck-tested: true source-blocks (AV, casting selection) hold even on a
package-locked Cicero; the un-blocked channels (headtrack, crouch) get out-fought
by the package — because they are not blocking yet. Today only `Headtrack` (ch.5)
is known-incomplete. Never mislabel a re-assert as a clean gate. (ch.20 `TargetPin`, ABI v13, is a
SOURCE block, not a re-assert: it answers the engine's own target-selector seat, so the engine's pick
never reaches the controller -- #0 (f) condition 4. It is not on this list.)

**#3 — Never substitute the package — SCOPED to the movement-hijack channels.** A
channel that commandeers a facet BY DRIVING IT OVER A RUNNING PACKAGE (the movement
hijack: locomotion/facing) must leave the actor's current package current and
evaluating (design.md §5). Making another source the current package fires
`OnPackageEnd`/`OnPackageChange` and tears the preempted source down; truthful state
cannot save it. Those channels keep the package coherent; the arbiter logs PACKAGE
STABLE ~1/s so a regression is visible.

**THE ONE INTENTIONAL EXCEPTION — the 0x49 package-OFFER channel (design.md §5a).**
The `CheckForCurrentAliasPackage` (vfunc 0x49) offer channel deliberately REDIRECTS the
alias-tier package OFFER to a client's own package, so the engine then runs the CLIENT's
REAL package NATIVELY. This is a package-tier PROMOTE, not a movement hijack, and it is
allowed precisely because it costs exactly ONE `OnPackageChange` each way — the SAME
class of interruption as vanilla combat taking an NPC and handing it back, which every
follower already tolerates. It is bounded: engage only in a gambit-valid-and-live window,
relinquish cleanly so the framework package resumes, and touch NO alias/run-once state.
It is STRUCTURALLY BENEATH script-driven (PapyrusUtil) overrides — it cannot reach them
(#3a) — so it can never break a script-driven follower. (Movement-hijack channels still
obey the no-substitute rule above; the offer channel is the one facet where a promote is
the correct, bounded mechanism.)

**#3a — APMF is BENEATH script-driven package overrides; it uses ZERO Papyrus.** APMF
arbitrates the engine's NATIVE / alias tier (packages the engine itself selects, and the
alias-tier package offer at 0x49). A PapyrusUtil `AddPackageOverride` sits ABOVE that
tier: the script layer wins, and APMF neither sees nor touches it — so "never break a
custom follower" is AUTOMATIC for any script-driven follower. APMF calls no Papyrus
itself. **Never-break guardrails (all three hold for every control window):** (1) control
only in BOUNDED, gambit-valid-AND-live windows — never a standing hold; (2) RELINQUISH
cleanly so the framework's package resumes; (3) the offer path touches NO alias / run-once
state. **Supporting evidence (Tuxborn audit, 2026-09-02):** across all 1626 enabled mods,
Simple Follower Framework + every custom follower are alias-tier with ZERO PapyrusUtil
package overrides — so the alias-tier (0x49) mechanism covers every follower in that list,
and the beneath-script layering means even a hypothetical script-driven follower is safe by
construction.

**#3c — A CAST IS NEVER A PACKAGE; a `kIntent_Cast` claim is TTL-BOUNDED.**

> **AMENDED 2026-09-07 — read this before the body of the rule.** #3c was written for the
> 2026-09-04 mechanism and was never updated when that mechanism was replaced on
> 2026-09-05, so as written it contradicts `#0(d)` and `#20` in this same file and the
> shipped code. Three corrections, in force:
>
> 1. **"denies, never drives" and "the CLIENT executes its own animated cast" are
>    RETIRED.** A `kIntent_Cast` claim COMPOSES (#0 action (d), #20): APMF answers the
>    engine's own cast-decision seats (0x06/0x07/0x0A/0x0D on the Restore + Offensive
>    caster vtables, plus 0x0F) and the NPC's OWN AI selects, equips, charges, aims,
>    fires and channels the claimed spell. APMF still makes NO cast write of its own —
>    that part of the rule stands, and it is the part #0 is really about.
> 2. **The T1 cast-leaf deny is NOT part of this claim any more.** `kIntent_Cast` no
>    longer maps to `kCombatActionCat_Cast`, and the `ContextMagic` CreateContextNode
>    hooks are removed outright. Denying the cast leaves under a cast claim would silence
>    the claim's own delivery. Those leaves are still denied for `kIntent_CombatAction`,
>    a separate live intent.
> 3. **The TTL is a bounded FLOOR, not a hard expiry, once claim-renewal lands.** Sub-rule
>    (a) below says "a longer stream is a NEW bounded claim, never a re-assert of the same
>    one". That sentence is the written origin of a real field failure: the claim died at
>    6 s, the client had not yet re-requested, and foreign spells equipped and charged in
>    the 0.3-1.2 s gap, every 6 s (MFO `DIAG-2026-09-06-deny-heal-failures.md` RC2;
>    `DENY-COMPLETENESS-AUDIT.md` open gap 11; engineering principle 9, "a floor is safe,
>    an expiry is not"). The correction is that a RE-REQUEST for the same {actor, spell,
>    hand} RENEWS the deadline instead of racing it — which keeps everything sub-rule (a)
>    is actually protecting (a crashed or forgetful client still auto-releases at the
>    deadline; nothing becomes a standing hold; there is still no re-assert loop).
>    The renewal is **MERGED TO `main` 2026-09-07** (branch `fix/apmf-claim-renew-denyhand-spellsteer`), CI-green, NOT yet in a tagged release and NOT yet deck-run. This amendment is what licenses it: without
>    amending #3c first, the rules file forbade the fix. The gap remains live in every
>    TAGGED release (v0.9.2 and older), and it only closes in behaviour for a client that
>    RE-POINTS its claim rather than releasing and re-requesting.
>
> Everything below is the original 2026-09-04 text, kept because sub-rule (b)
> (`kCastFlag_FromPackage` reads, never runs) and the bounding discipline are unchanged.

The cast-execution facet (ch.8b, `channels/CastCompose.cpp`, ABI v5
`kIntent_Cast`) is the keystone's category correction (design.md §0/§3): a heal/ward/buff
the AI would not choose is a CAST facet, not a package facet. A `kIntent_Cast` claim does
EXACTLY what every other claim does — arbitrate + DENY — through the SAME three gates
cast-select already rides (0x0A CheckCast, 0x0F CheckShouldEquip via `Allowance::AllowedCast`;
the T1 cast leaves via `kCombatActionCat_Cast`), adding NO engine call. APMF makes NO cast
write for it (no `CastSpellImmediate`/`StartCast`/`InterruptCast`/`NotifyAnimationGraph`/
`EquipSpell`/`selectedSpells`) — the CLIENT executes its own animated cast; APMF only keeps
the AI's competing cast/re-arm out of the way and never touches movement (design.md §3.7).
Two hard sub-rules: (a) **the claim is ALWAYS bounded** — `expiresMs` is set on engage
(default 4 s, clamped to 15 s) and the `ControlMap` Drain TTL pass AUTO-RELEASES it at
expiry; this is a release, the OPPOSITE of a re-assert (#1), so a crashed/forgetful client
can never leave a standing cast hold. A longer stream is a NEW bounded claim, never a
re-assert of the same one. (b) **`kCastFlag_FromPackage` reads, never runs** — APMF extracts
ONLY spell+target out of the handed package and NEVER offers/installs/evaluates/runs it
(`PackageGate`'s 0x49 thunk reads only `kIntent_OfferPackage`, so a cast claim is invisible
to it by construction). If the pinned CommonLib cannot cleanly read the package data, the
extraction REFUSES (never a package run) and the client passes the spell directly (design.md
§5.1/§6). The one intentional package-tier path stays #3's 0x49 offer, for REAL native
packages only — a cast never touches it.

## Threading

**#4 — The control map's WRITE side is main-thread-only state; the READ side is an
RCU snapshot readable from any thread.** `ControlMap`'s working map, `m_index`, and
every channel's engine mutation + per-NPC state map are mutated ONLY on the WRITER
thread — in `Drain()` (applying enqueued ops), `ReleaseAll()`, `Clear()`, and every
channel's `Engage`/`OnOwnerChanged`/`Release`; the input sink
(`BSInputDeviceManager`), the PlayerCharacter `0xAD` seat, and the SKSE revert/
preload callbacks all dispatch there and are serial (confirmed the same MAIN thread
by the `[threadcheck]` evidence — see #12). Do NOT mutate the map from any other
thread. The ONLY shared-with-workers state is the request `m_queue` (guarded by
`m_qmx`) and the atomic handle counter. The per-NPC hot path (`OnActorUpdate`) is
NOT confined to the writer thread (field-proven — #12) and READS a published,
immutable RCU snapshot with no lock, one sanctioned per-entry exception aside — see
#12/#13.

**#5 — Release restores what engage changed.** Any channel that writes engine state
(AVs, the spell slot, weapon state, AI-driven flag) must capture the prior value at
engage and restore it in `Release`. `Arbiter::ReleaseAll` runs on disengage,
target-unload, and `kPreLoadGame` — never skip or reorder it, or the actor keeps the
mutated state across a save load.

**#5a — an ARBITRATION / DENY channel RELINQUISHES on release; it never "undoes" a
live engine decision.** An arbitration-only channel (ch.6 combat-target, ch.14
shout-power; and ch.8 casting, whose gates deny but whose channel still writes
nothing) wrote nothing to the engine (#0), so its `Release` has nothing to restore —
it just drops the claim record. And a DENY channel over a self-correcting engine
decision the AI keeps re-making (a combat target, once a real deny gate exists) must
NOT reverse that decision on release (no `StopCombat`): clients release such a claim
CONSTANTLY (a gambit yields, an expiry sweep, a target switch), so undoing on each
release would yank the actor out of an ongoing fight and flicker on a switch. Let the
engine keep the decision if it is still valid and end it for its own reasons otherwise
("commanding WHICH foe is ours; commanding THAT there is a foe is not"). This is #5's
counterpart: a channel that SET a stored prior value restores it (#5); an arbitration/
deny channel relinquishes.

## Version robustness

**#6 — Version-robust hooks only; VR-refused.** Hook VIRTUAL vtable indices
(`Actor::Update` `0xAD`), never non-virtual call-site offsets (that is the whole
reason `0xAD` is safe — design.md §3, §8). Install once, idempotent. VR is refused
at install: the `0xAD` index is unverified for VR (`REL::Module::IsVR()` guard),
and Address-Library IDs like `StartCombat` have no sourced VR id.

**#7 — Guard + log every struct-member write.** Struct offsets
(`AIProcess.currentPackage`, `selectedSpells[]`, `movementController`,
`combatController`, `caster->currentSpell`) are more version-sensitive than vtable
indices. Reach them through CommonLib accessors and null-check the accessor before
writing. Rely on CommonLib's build-time `static_assert`s; never hand-write an
offset.

**#8 — Pinned CommonLib API surface (colorglass rev).** The probe confirmed this
rev does NOT bind some functions the design references:
- `Actor::StartCombat` — not bound. **APMF does NOT call it (#0 — that is behavior; it
  is the CLIENT's job).** Recorded as a fact only: its REAL signature is
  `bool(RE::Actor*, RE::Actor*, void*)` — THREE args (a 2-arg wrapper faults inside the
  engine, reading the 3rd param from a garbage register -> a hard AV; that CTD is why
  ch.6 is now arbitration-only). A client that initiates/commands a combat target does
  so itself (e.g. MFO's own `currentCombatTarget` compare-and-write + its StartCombat).
- `Actor::SetCurrentSpell` — not bound (only a no-op `SetCurrentSpellImpl`); a CLIENT
  that owns cast selection writes `selectedSpells[slot]`/`caster->currentSpell` itself.
  APMF's ch.8 does NOT write it (#0) — it denies every other spell at the two gates instead
  (`CastGate.cpp:124`, `EquipGate.cpp`; "arbitration-only" here was corrected 2026-09-07). Engine fact (deck-confirmed, useful to
  the client): writing `selectedSpells[slot]` + `caster->currentSpell` directly (guarded)
  makes the AI KEEP that selection and cast it as its own decision — the client's path
  to a real animated cast; APMF just arbitrates the facet.
- Use `actor->AsActorState()` for attack/weapon/block state, not raw members.
- `MovementControllerNPC` exposes NO named AI-driven setter in this rev — only
  unnamed `Unk_0C/0D` void(void) vfuncs (calling them blind is the documented
  CTD roulette). Movement FULL block (ch.1) therefore uses the Address-Library-bound
  `SetDontMove` (`RELOCATION_ID(36490, 37489)`) to lock translation PLUS
  `KeepOffsetFromActor` (`RELOCATION_ID(36870, 37894)`) / `ClearKeepOffsetFromActor`
  (`RELOCATION_ID(36871, 37895)`) to null the move INTENT at the source — none is
  bound in CommonLib (CommonLibSSE-NG does not vendor `KeepOffsetFromActor`). The
  KeepOffset IDs are VERIFIED: cross-checked against shipping SKSE source with the
  identical signature that also reproduces the `SetDontMove` anchor verbatim, with
  zero conflicting values found — safe to ship.
- `Actor::StartCombat` — not bound; combat-target STEER (ch.6) uses
  `RELOCATION_ID(37608, 38561)`, signature `void(Actor*, Actor* target)`. VR-refused
  (SE/AE IDs). `StopCombat()` IS a bound vfunc (0xE5).
- Bound and callable directly (verified against the fork): `ActorEquipManager::
  GetSingleton/EquipObject/UnequipObject/EquipShout`, `Actor::GetEquippedObject(bool)`,
  `DrawWeaponMagicHands(bool)`, `PauseCurrentDialogue()`, `IsSneaking()`,
  `NotifyAnimationGraph(const BSFixedString&)`, `AIProcess::PlayIdle` /
  `SetHeadtrackTarget(Actor*, NiPoint3&)`.
- `Actor::StopCurrentDialogue` does not exist; the real vfunc is
  `PauseCurrentDialogue()` (0x4F). `SetDialogueWithPlayer` is
  `(bool, bool, TESTopicInfo*)`.
- `movementController` is a `BSTSmartPointer` — reach the raw pointer with
  `.get()`; `combatController` is a raw pointer.
Match this surface; do not assume an unbound function exists. Every name here was
verified against the pinned rev's headers via CI (never from memory).

## Build / registration

**#9 — Channels self-register; keep every source in the DLL target directly.**
`APMF_REGISTER_CHANNEL` relies on a file-scope static initializer running at load.
That happens ONLY because CMake GLOBs every `.cpp` directly into the plugin DLL
target — NOT via an intermediate static archive, which would strip unreferenced
initializers and make channels silently vanish. Never wrap `channels/` in a static
library. Registration order is load-order-undefined; never assume a channel index or
ordering.

**#10 — Adding a channel touches one file (+ maybe one appended enum value).** One
new `channels/*.cpp` that subclasses `Channel`, declares its `ServesIntent()`, and
ends with `APMF_REGISTER_CHANNEL`. No edit to CMake, the registry, the input layer,
or the arbiter. If the facet needs a NEW client intent, APPEND one value to the
`Intent` enum in `APMF_API.h` (that is the ONLY permitted edit outside the channel
file, and it is append-only — see #14). If a change would require editing the core
to add a facet, the abstraction is wrong — fix the abstraction, not the core.

## Coherence / eviction

**#11 — Momentary channels hold no state; `Release` is a no-op.** A one-shot with no
lasting authority (`Dialogue` pauses once; `Idle` fires one animation) does its work
in `Engage` and leaves `Release` empty — there is nothing to restore. It still holds
a claim in the control map until released (the tester's second press, release-all,
or the client's `Release`); that claim is inert. Do not make a momentary channel
override `Tick` or capture per-NPC state.

## Multi-NPC control map (Phase 1)

**#12 — RCU SNAPSHOT; single-writer, multi-reader.** [threadcheck] field evidence
(2026-09-02) proved the `0xAD` hook does NOT run on one serial thread: a `Character`
seat fired on a DIFFERENT worker thread than the `PlayerCharacter`/Drain seat. The
old "single-writer, unlocked-read" model that assumed one serial game thread for
BOTH sides was therefore FALSE and has been retired. What is still true, and is now
the load-bearing assumption instead:

- The WRITER side is single-threaded. `Drain()` (from the `PlayerCharacter` `0xAD`
  seat), `ReleaseAll()` (from `kPreLoadGame`), and `Clear()` (from `OnRevert`) all run
  on the same MAIN thread — this is exactly what `[threadcheck]` verifies: it records
  the `PlayerCharacter`/Drain seat's thread id once and would fire if a `Character`
  seat's thread ever matched it AND something else also mutated the map off that same
  thread; in practice only the per-NPC `Character` seats are parallelized across
  worker threads, never the writer path. A channel's OWN per-NPC state map is
  likewise writer-thread-only (`Engage`/`OnOwnerChanged`/`Release`).
- The READER side (`OnActorUpdate`, i.e. every `Character` `0xAD` seat) is NOT
  single-threaded, and is NOT assumed to be. It must never touch writer-thread state
  directly.
- Client API calls (`Request`/`Release`/`Repoint`) may arrive from a client's worker
  thread (MFO's BSJobs worker) — unchanged: they only ENQUEUE a small POD op under a
  brief lock on `ControlMap::m_queue`; they NEVER touch the map.

The fix is an RCU snapshot, not a hot-path lock: the writer keeps a private working
copy (`m_current`) built from the last published generation; `Drain()`/`ReleaseAll()`/
`Clear()` apply ops/the unload sweep/a wipe against that private copy and, ONLY if
something actually changed, atomically PUBLISH it —
`m_published.store(newSnapshot, memory_order_release)` — as a NEW immutable
`std::shared_ptr<const MapType>` generation (copy-on-CHANGE: a no-op frame allocates
nothing). Readers atomically LOAD a LOCAL `shared_ptr` copy of `m_published`
(`memory_order_acquire`, pairing with the writer's release) and read that frozen
generation lock-free: no torn reads, no UAF even if the writer publishes a newer
generation mid-call — the reader's local `shared_ptr` keeps its own generation alive
via refcount. `m_index` needs no snapshot wrapper (writer-thread-only, no reader ever
consults it) and stays a plain map. Handles are allocated with an atomic counter so
`Request()` returns synchronously before its op is drained; ops are FIFO so a
`Release` enqueued right after its `Request` is applied after it.

Per-entry mutation: every `NpcCtl` field is READ-ONLY to readers once published, with
ONE sanctioned exception — `obsTick` (the per-tick observability counter) is a
`mutable std::atomic<uint64_t>` (relaxed) so `OnActorUpdate` can `fetch_add` it
through the `const NpcCtl&` a snapshot lookup hands back, without breaking the
"snapshot is otherwise immutable" guarantee. No other field may ever be
reader-mutated — a future channel's `Tick()` must not write anything else in
`NpcCtl`/`ChannelCtl`/`Claim`; if it needs live per-claim data, give it the same
atomic treatment, don't reach into the map raw.

Lock-freedom of `std::atomic<std::shared_ptr<const MapType>>` is NOT assumed — it is
disclosed once at hook install (`Hook::Install`, via
`ControlMap::SnapshotIsLockFree()`). Not-lock-free (spinlock-backed, plausible on
MSVC) is ACCEPTABLE: the control map holds only a handful of controlled NPCs, so the
critical section is a pointer copy, not a hot loop — but it must show up in the log,
never be assumed silently.

Break any of this — mutate the published snapshot in place from a reader, mutate
`m_current`/`m_index` off the writer thread, or reader-mutate any `NpcCtl` field
besides `obsTick` — and you get a data race across hundreds of NPCs.

**#13 — An uncontrolled NPC pays near-zero.** `OnActorUpdate` runs for EVERY NPC
every frame, on whatever thread the engine schedules it on (#12). It must do at
most: a relaxed `m_anyControlled` check (no atomic `shared_ptr` traffic at all while
nothing is controlled), then one acquire-load of the published snapshot + ONE hash
lookup that misses — no allocation, no iteration over all NPCs, nothing else. Only a
CONTROLLED NPC runs its channels' `Tick` (and most channels no-op there — a clean
block does no per-tick work, #1). Never scan all NPCs, never allocate on the hot
path. The liveness sweep and queue drain run once per frame over the SMALL control
map (the writer's private working copy), never over all actors, and publish a new
snapshot only when something changed.

## API contract

**#14 — `APMF_API.h` is a frozen, APPEND-ONLY C-ABI contract.** `APMF_API.h` is the
ONLY file a client shares with APMF; a client (MFO) and APMF compile as SEPARATELY
built DLLs and interact ONLY through it at runtime. So the surface is C-ABI: a POD
struct of function pointers (`APMF_API_v1`) with POD args only (`RE::FormID`, the
plain `Intent` enum, floats) — NO C++ class, NO STL, NO vtable crosses the boundary.
Once shipped, NEVER change or reorder an existing field, `Intent` value, or
function-pointer slot; only APPEND (new `Intent` values at the end, new fields at the
END of a struct, bump `kABIVersion`). A client built against v1 must keep working
against every later APMF (same discipline MFO applies to `MEO_API.h`). The interface
is handed over by the exported query function `APMF_GetInterface` (chosen over the
SKSE-messaging handshake: synchronous, no message-ordering/routing subtlety). APMF
holds ZERO client-specific code — delete every client and APMF loses zero lines; the
header + the query function are the ONLY seam. NO EXCEPTION MAY CROSS THE BOUNDARY:
each exported body (`APMF_Request`/`APMF_RequestEx`/`APMF_Release`/`APMF_GetInterface`)
is wrapped in a `try { … } catch (...)` returning `kInvalidHandle`/void/`nullptr` — a
throw (bad_alloc from the queue, an spdlog throw) unwinding across the client's
separately compiled DLL is UB. A swallowed throw degrades to "no control taken",
never a crash.

**#14b — WHEN `kABIVersion` BUMPS, and why a bump is expensive (practised rule,
written down 2026-09-07).** The header is the ONLY canonical statement of the current
number — read `kABIVersion` in `native/APMF_API.h`. **No other document may restate it**,
this rule included: four did, and by 2026-09-07 they had drifted to 2, 3, 3 and 4 while
the header had moved on. A restated version number is a copy that cannot be kept in sync.

- **A new struct or a new function-pointer slot ⇒ BUMP.** That is what `abiVersion >= N`
  guards: a client must be able to tell whether the slot it is about to call exists.
- **A new BIT in an already-frozen word ⇒ DO NOT BUMP.** `APMF_CastFlags`' stop-percent
  bits 8-15, `kCastFlag_DualCast` and `kCastFlag_DenyHandOnly` all added meaning to a
  field that already shipped, at the same offset, with the same size. An older APMF
  reads the bit as 0 and behaves exactly as it did before — the degrade is automatic
  and correct, and no version test could improve on it.
- **The exception: a bit whose silent ignore IS the failure.** The ch.19 gait bits
  (`kTravel_SpeedSet` + speed, 2026-09-24) are new bits in the frozen `ival` word, but an
  older APMF ignoring them walks the actor at the wrong speed with no refusal and no log,
  which is a masked failure (CLAUDE.md principle 7), not a correct degrade. Such a bit
  rides a bump that exists anyway (here v12, for `GetTravelLegState`) and its doc tells
  the client to gate it on `abiVersion`. Never bump for such a bit alone without asking
  whether the degrade is really wrong.
- **Why the asymmetry matters HERE.** `APMF_GetInterface(v)` returns **nullptr** when
  `v > kABIVersion` (`core/ClientAPI.cpp:125-128`), and MFO calls
  `fn(APMF_API::kABIVersion)` ONCE, with no downward retry: on null it logs "APMF
  refused ABI v{} (too old) -- owned-cast model OFF" and disables the whole owned-cast
  model (`native/APMFBridge.cpp:398-403`). So a gratuitous bump does not degrade a
  feature — it turns the reference client's entire cast integration off against every
  older APMF.dll in the field. Bump when a slot genuinely needs a version test; never
  as bookkeeping.
- **PROPAGATION.** The header is byte-shared. A change to it lands in MFO's
  `native/APMF_API.h` in the SAME deploy pair, and `md5sum` must match on both repos at
  every tag and every deck deploy — the header is the one file where "my branch is
  ahead" is a shipping bug, not a merge detail. Verify before tagging either side.

**#14a — ABI revisions use PREFIX EXTENSION; the param payload is POD, append-only,
and NEVER retained.** A new ABI revision (v2: `RequestEx` + `APMF_Param`; v3:
`Repoint`, which re-points an existing claim's param in place, same handle) adds a
struct `APMF_API_vN` whose LEADING members are byte-identical, in order, to
`APMF_API_v(N-1)`, then appends the new function-pointer slots. The SAME static
object is handed to every client: `APMF_GetInterface` returns the base type
(`APMF_API_v1*`); a newer client checks `p->abiVersion >= N` and reinterpret_casts up
to `APMF_API_vN`. Never edit a shipped `APMF_API_vN` struct — add the next one.
`APMF_Param` is a PLAIN POD struct (`form`/`fval`/`ival`), never a class/STL/pointer-
to-owned-memory; it too is append-only (new fields at the END, so a v1-era caller's
zero-init reads them as 0). An ALL-ZERO param (`{}`), and the `Request`/no-param path,
means "channel default" — cast-select falls back to Firebolt, combat-target to the
player — so v1 behavior is preserved exactly. The `const APMF_Param*` a client passes
to `RequestEx` is READ AND COPIED synchronously inside the call (into the queued POD
op); APMF NEVER retains the client pointer, so a client stack temporary is safe.

## Persistence

**#15 — Persisted AV overrides are CO-SAVED; never stranded.** The AV channels
(Attribute/Speed/Detection) mutate dynamic ActorValues that persist in the `.ess`. A
RAM-only "prior value" would be STRANDED if the player saves while engaged and
reloads (after a load the live control map is empty, so a plain Release restores
nothing — MFO's "fix-forward never cleans old saves" class). So EVERY persisted-AV
write goes through the co-saved ledger `core/AvLedger` (`av::Override`/`av::Restore`),
NOT raw `SetActorValue`. The ledger records `(FormID, AV) -> captured prior value`
and is co-saved via SKSE serialization (unique ID `'APMF'`, record `'AVOV'`): `OnSave`
writes it, `OnLoad` reads it into a pending set (`ResolveFormID` for load-order
remap), `kPostLoadGame` restores each AV regardless of live engaged-state and clears,
`OnRevert` wipes ledger + control map (no restore — actors are being replaced). Any
NEW channel that writes a persisted actor value MUST route it through the ledger.

RECORD VERSIONING — a reader per version, KEPT FOREVER. The `'AVOV'` record carries
`kRecordVersion`; `Load(intf, version)` branches on it. NEVER change a record's byte
layout under an existing version number — bump `kRecordVersion` and add a reader
branch, or an old save's entries misalign (e.g. reading a 12-byte v1 entry as a
16-byte v2 entry consumes the next entry's FormID as `applied` → every override
stranded). Shipped layouts:
- **v1** (12 B/entry): `{FormID, ActorValue, prev}` — restored UNCONDITIONALLY (no
  `applied`; the clobber guard cannot apply — v1's original semantics).
- **v2** (16 B/entry): `{FormID, ActorValue, prev, applied}` — clobber-guarded.
`OnSave` always writes the current version; a `version > kRecordVersion` record is
skipped (a downgrade cannot read a future layout).

**SECOND RECORD, added 2026-09-23 (ABI v11): `'XMRK'`, the APMF MARKER LEDGER**
(`core/PositionCast.{h,cpp}`). Same unique ID `'APMF'`; `plugin.cpp` `OnLoad` dispatches by
record type, so each record keeps its own version line and neither layout touches the other.
- **v1** (16 B/entry): `u32 count`, then `count x {u32 formID, f32 x, f32 y, f32 z}` -- every
  XMarker APMF placed and has not deleted (a stale handle keeps its entry).
Same rules as `'AVOV'`: a reader per version forever, a newer record skipped and logged, no
layout change under an existing number. Timeline: revert callback clears the ledger; load
callback reads the record (no ref lookups there); kPostLoadGame POSTS the sweep, and the first
main-thread pump after the load deletes each recorded marker
that passes ALL THREE PROOFS (0xFF FormID resolves, XMarker base, recorded position within
1u, not already deleted), FORGETS any that fails one (never touched), and CARRIES any not in
memory to later loads (cap 64). A save without the record (0.9.7 and older) sweeps nothing.

CLOBBER GUARD: the ledger stores both the captured `prev` AND the value we `applied`;
Restore/ApplyPending write `prev` back ONLY when the AV still equals `applied`. If a
quest or another mod changed the AV while our override was live, the newer value
wins and we just drop our stale record — APMF never silently overwrites another
author's write. ONE-CHANNEL-PER-AV assumption: the ledger keys by `(FormID, AV)`, so
at most one APMF channel may own a given AV on a given actor (true today: the three
AV channels own disjoint AVs). Sharing an AV would need per-AV refcounting.

SAVE-SAFETY BOUNDARY: only the AV channels are co-saved and therefore save-safe.
Transient facets (weapon draw, sneak, dialogue, idle, headtrack, combat-target) are
self-correcting and need no co-save. **Equipment (ch.15) is the exception that
matters: it mutates PERSISTED inventory (unequip) but its re-equip pointer is
RAM-only (NOT co-saved).** So a client must NOT hold an equipment claim across a
save — it self-heals (item stays in inventory, the AI re-equips), but APMF does not
restore it. Do not add a persisted-state channel without either co-saving it or
documenting the same boundary.

**#19 — A RUNTIME-MINTED FORM MUST NEVER BE REACHABLE FROM A SAVE, AND MUST DROP
ITS BORROWED POINTERS BEFORE A LOAD.** The cast drive mints delivery-flip proxy
`SpellItem`s through `IFormFactory` (`core/CastProxy.{h,cpp}` -- renamed from
`CastExecutor` when the forced drive was retired; the pool outlived the drive because
the engine's `FindTargets` Self branch always lands on the caster).
These are dynamic `0xFF` forms: they do NOT survive a load, and the engine purges
them on the way in. Two rules follow, both learned the expensive way (MFO's
`native/Actuation_Direct.cpp` paid for the second one first):
- **Never let one be captured into the `.ess`.** A proxy is `AddSpell`'d to the
  caster only so the AI's own equip selector can choose it (and so the
  `CombatInventory` rebuild makes an item for it), and a save taken while it is known
  would persist a reference to a form that will not exist on the next load. So the
  transient is un-taught when the claim releases (`castproxy::Free`, the single choke
  point) and swept unconditionally on the SKSE save callback
  (`castproxy::PreSaveSweep`, called from `plugin.cpp`'s `OnSave` BEFORE any record is
  written).
- **Clear borrowed pointers before the purge.** The proxy shares the SOURCE spell's
  `Effect*` objects BY POINTER (that is the whole point — same effects, flipped
  delivery). If the dead proxy still holds them at load time, the purge frees a
  LIVE spell's effect array through it. So `castexec::ResetAll` clears each form's
  `effects` FIRST, then nulls the slot, and it runs on BOTH the revert callback and
  kPreLoadGame. (`castexec::ResetAll` is now `castproxy::ResetAll`.) This is also what stops the fixed-size proxy pool from staying
  permanently occupied by a stale owner across a revert (`ControlMap::Clear()`
  deliberately does not call `channel->Release`, so nothing else would reset it).
Any future subsystem that mints a runtime form inherits both halves of this rule.

## Logging

**#16 — Log hex via `apmf::log::Hex`, never the `{:X}` spec.** On the deck (v0.2.2)
APMF's build rendered EVERY `{:08X}`/`{:02X}`/`{:X}` as raw garbage bytes, corrupting
the log to a binary file (grep refused it) — while decimal `{}` and string `{}`
rendered clean, and the IDENTICAL toolchain/baseline formats `{:X}` correctly for MFO
(its `MFO.log` shows clean `FE08F801`). So the trigger is APMF-build-specific and was
not statically isolable (it is not a format-string typo, a non-ASCII byte, an arg-type
slip, a config/PCH/preset/baseline difference, or a custom formatter — all ruled out).
The robust fix, immune to whatever the trigger is: format hex BY HAND into an ASCII
`std::string` (`core/Log.h` `Hex(value, width)`) and log it through the string path
(`spdlog::info("… 0x{} …", apmf::log::Hex(id))`), which both projects render correctly.
NEVER reintroduce a `{:X}`/`{:x}` presentation spec in a log call; use `Hex`. If the
underlying cause is ever found and fixed, this rule can relax — until then it keeps the
log (the primary field-validation channel) plain text.

## Framework coexistence

**#17 — APMF must coexist with arbitrary modlists: it is a good citizen, never assumes
exclusive access, and degrades rather than crashes.** APMF lives inside OTHER people's
modlists, alongside mods it has never seen and cannot enumerate — it must never be the
thing that derails one. Concretely:
- **Vtable hooks (`write_vfunc`) ONLY.** They chain cleanly — another mod hooking the
  same slot wraps the previous entry and calls the original, so independently-authored
  hooks compose without coordination. **NO raw call-site patches**
  (`write_call`/`write_branch`/a hand-rolled 5-byte overwrite/trampoline at a shared
  address): they stomp bytes at one address, and two uncoordinated patchers at the same
  site is a collision, not a composition. **Cautionary case:** the T4
  `TESActionData::Process` probe's devirtualised fallback patched valhallaCombat's
  known call site with `SKSE::GetTrampoline().write_call<5>` — SCAR.dll patches the
  SAME AI-attack-start site, and the two collided into an execute-AV CTD in ordinary
  combat (2026-09-03, not even during a probe keypress; see `Docs/PROBE-ALLOWANCE.md`
  "T4 — DEFERRED" for the full crash record). T4 was removed rather than patched
  around — this is the standing reason why. See also #6 (call-site offsets are also
  version-fragile; this is a second, independent reason they're banned).
  **#17a — the ONE bounded call-site exception (marth 2026-09-15): a FACET WITH NO VIRTUAL
  SEAT, taken in its ENTIRETY.** Amended after the engine-equip design pass: every engine
  equip funnels through the `ActorEquipManager` worker (AE 38929→0x6CBE30 / SE
  37974→0x639E20), reached ONLY from two internal call sites (`EquipObject` 38894+0x170 /
  37938+0xE5 and the non-queued sibling 38893+0xBC / 37937+0xBC; full E8 scan), and NO
  virtual function exists anywhere on that path (`Actor::AddWornItem` is devirtualised).
  A facet like that cannot be seated by a vtable hook at all, so "vtable only" would mean
  "no control", which is the opposite of APMF's purpose. marth: "an exception would be
  measured control of that facet in entirety … a command is sent to APMF with what to
  equip and that is enforced until overridden." Conditions, ALL required:
  (1) the site is an INTERNAL call site inside a non-virtual choke point — never a public
  entry another mod would reasonably patch (the two sites above are inside the engine's
  own `EquipObject` bodies, not at a caller);
  (2) install byte-verifies the exact 5 bytes at EACH site as `E8 rel32 → worker` on the
  running binary; any mismatch REFUSES THE WHOLE SEAT and logs it — a SCAR-class
  collision becomes a refusal, never a CTD;
  (3) per-runtime ids AND offsets come from disassembly of that runtime (rule 11), never
  from a CommonLib declaration;
  (4) engine-answer-first is preserved in the only form a sink allows: deny = do not call
  the worker; the seat NEVER manufactures an equip the client did not declare — the
  client's declared set is what APMF equips, and the seat is what keeps the engine from
  undoing it;
  (5) the seat is scoped to actors holding an explicit claim (`kIntent_EquipAuthority`);
  the player and unclaimed actors pass through untouched and unlogged.
  Under #17a the facet is taken WHOLE (declare the worn set → APMF equips it and refuses
  everything else until the client re-declares or releases), which is full control of the
  facet exactly as a per-path deny would be. Any further call-site seat needs its own
  #17a argument, made explicitly. See `Docs/DENY-COMPLETENESS-AUDIT.md` row 17 and
  `MAP.md` EquipSink once they land.
- **Engine-answer-first.** Every thunk calls the stored original before deciding
  anything; it only ever flips the engine's own YES to NO, only for an actor APMF
  itself holds a claim on. Never invent a YES, never manufacture behavior, never
  re-assert (#0). (ch.22's slot-0x99 seat changes `IsDead`'s answer at `StartCombat`'s own
  self-check only; in `StartCombat`'s terms that is its YES turned to NO, before it acts -- #0 (h).)
  **ONE bounded exception exists in the whole codebase** — ch.8b's
  `CheckShouldEquip` seat, where the original is structurally un-redirectable and
  chaining would answer nothing at all. #20 states its three conditions; a second
  exception needs the same argument made explicitly, not an appeal to the first.
- **Verify at install.** RTTI-derivation check per vtable symbol before installing
  (`core/Allowance.h`'s `DerivesFrom`, the `CombatMagicCasterArmor` lesson) — a symbol
  that doesn't derive the expected class is skipped, not installed blind. At the hot
  path, a foreign/unrecognized object (a vtable APMF never installed on reaching the
  thunk) returns the benign default without touching its members.
- **Read, don't own.** Consult engine/APMF state via the lock-free RCU `ControlMap`
  snapshot (#12); never assume exclusive ownership of an actor, a vtable, or a call
  site just because APMF is loaded.
- **ADAPT, don't degrade; never crash.** When a preferred attach point is contested by
  another mod, absent on a runtime, or devirtualised, ROUTE TO AN ALTERNATIVE SEAT that
  covers the same facet — do not simply disable. The template is designed with
  REDUNDANT per-facet attach points to make this possible: choose the viable seat at
  install time (RTTI-verify, confirm the vtable/site is usable and not already
  stomped), and fall back to the next when the preferred one isn't viable. Examples of
  the redundancy: combat ACTIONS are covered by BOTH T1 (combat-tree leaves, chain-safe
  vtable) AND T4 (action-data) — so T4's call-site seat colliding with SCAR does NOT
  lose combat-action coverage, it falls through to T1; CASTING is covered by T1 Cast
  leaves AND T2 `CheckCast`. Only when a facet has NO usable seat is it disabled (a
  documented coverage gap), and even then APMF logs and continues — never a bad call,
  never a crash.

The current codebase satisfies this: every hook in `native/core/` is `write_vfunc` on a
version-pinned `VTABLE_*` symbol, with ONE exception, the #17a seat: `core/EquipSink.cpp`
is the only `write_call`/`AllocTrampoline` in `native/` (2026-09-15), and it exists only
under #17a's five conditions. No other call-site patch remains anywhere in the tree after
T4's removal.

## Deny completeness

**#18 — DENY COMPLETENESS: for EVERY facet APMF can ACCEPT a claim on, the deny
must ZERO the competing source across ALL paths it can reach that facet — a
partial deny is a TOTAL architecture failure, not an edge case.** If a client can
claim a facet, APMF must be able to reduce the competing source (native combat AI,
a foreign framework, a package) to ZERO influence on that facet — not "mostly", not
"the firing path but not the setup path". A residual competing write that still
reaches the facet is a RACE, and a race against a client that is concurrently
executing the behavior (e.g. MFO's forced equip on the main thread while the combat
AI runs on the combat thread) is a data hazard that CTDs, not a cosmetic flicker.
The rule is the explicit form of #0/#1: #1 says "block the foreign input"; #18 adds
"block EVERY foreign input path to the owned facet, and prove you enumerated them."

- **The obligation is per-facet path ENUMERATION.** Before shipping a claimable
  intent, enumerate every path the competing source can reach that facet (select,
  fire, SET-UP/context-creation, equip, re-arm, package-offer, …) and verify the
  deny zeroes each. A facet is DONE only when no enumerated path leaks. Record the
  enumeration in `Docs/DENY-COMPLETENESS-AUDIT.md` (intent | source paths | deny |
  complete? | gap) and keep it current as claim kinds are added.
- **The cast/equip lesson (2026-09-04, the rule's origin).** `kIntent_Cast` denied
  the cast-FIRING path (0x0A CheckCast, 0x0F CheckShouldEquip, the four T1 cast
  leaves) but NOT the cast-CONTEXT-CREATION path: the combat AI still BUILT its
  `CombatBehaviorContextMagic` / `CombatBehaviorEquipContext` (over a
  `NiPointer<CombatInventoryItem>`) and dereferenced the item UPSTREAM of the firing
  leaves, racing MFO's forced equip → `EXCEPTION_ACCESS_VIOLATION call [rax+0x28]`
  rax=0 (null item vfunc), MFO.dll frame 5. Addressed by extending `core/ActionGate.cpp`
  to also deny that context node (`apmf::cbt::kCastContextNodes`), classified
  `Cast|Offense`. The firing-only deny LOOKED complete because it stopped the visible
  cast; the CTD proved a setup path it never touched. (The act()-only form of that
  context-node deny was itself the next CTD — see the act()/pop() pair bullet below;
  the seat is right, the deny had to become the full pair.)
- **Completeness has a PER-HAND axis too, not just per-path (feat/deny-perhand).**
  A claim can name a hand (`kIntent_Cast`'s `CastFlags::kCastFlag_LeftHand`); "the
  deny zeroes this facet" must then mean "zeroes it for the CLAIMED hand only,"
  never a blanket both-hands suppression dressed up as complete. Resolve the
  hand from an engine-native signal AT THE SEAT (`MagicCaster::GetCastingSource()`
  for `CheckCast`; `CombatInventoryItem::itemSlot.equipSlot` vs.
  `BGSDefaultObjectManager`'s hand slots for `CheckShouldEquip`) — never invent one.
  A seat with NO native hand signal (the `ContextMagic` `CreateContextNode`'s
  `act()`: `CombatBehaviorTreeNode`'s fixed 10-vfunc layout and
  `CombatBehaviorTreeControl` carry none) stays PER-ACTOR and is a DOCUMENTED gap
  (next bullet), never silently presented as per-hand. See
  `Docs/DENY-COMPLETENESS-AUDIT.md`'s "per-hand pass" section.
- **A deny must honor the denied seat's OWN PROTOCOL — the act()/pop() pair
  lesson (feat/ai-cast-suppress, 2026-09-04, the recurring deck CTD).** Denying a
  behavior-tree node means making it FAIL the way the engine's own `ForceFail`
  node fails, and `ForceFail` is a PAIR: its `act()` (slot 0x02) pushes 4 bytes on
  the thread's data stack, sets failed, ascends; the runner then calls the SAME
  node's `pop()` (slot 0x03) in the same step, and `ForceFail::pop()` pops those
  4 bytes. Substituting only `act()` while the node's OWN `pop()` still ran
  unbalanced the data stack (push 4 / pop sizeof(node state) — 0x30 for the
  ContextMagic context node, which also released two NiPointers out of the
  enclosing frame and restored the context window from garbage) and corrupted the
  thread until an interrupt unwind walked a garbage `cur_node`
  (`call [rax+0x28]`, rax=0). "It stopped the visible behavior" was not proof of
  a correct deny — it was balanced by accident only for the 4-byte leaves. The
  rule: a deny at a paired seat installs BOTH halves (`core/ActionGate.cpp` hooks
  0x02 AND 0x03 and routes a denied node's next `pop()` to `ForceFail`'s own
  `pop()` via a thread-local pending record), refuses to arm if either half fails
  to resolve, and documents the measured protocol (`core/CombatBehaviorRE.h`
  "The node protocol") rather than inferring it from a class declaration. #17's
  "engine-answer-first, flip YES→NO" applies to the whole protocol, not one slot.
- **A deny of a RUNNING node uses the node's own failure exit — the update() half
  (ch.23 pursuit leash, `kIntent_PursuitLeash`, ABI v16, 2026-09-25).** A movement leaf runs its whole path
  inside ONE leaf (Advance's update ends only when its CombatPath completes or fails),
  so a deny that only refuses act() cannot stop a leaf that started before the deny's
  condition held — for a leash, every chase begun inside the radius. The leash
  therefore has a second seat, slot 0x04 (update, `(node, thread)`) on its 14 leaves (ten
  pursuit + four search), and ends a running leaf with exactly what the leaf's own update does when its
  path fails and what `ForceFail::act()` does after its push: the engine's
  `SetFailed(thread, 1)` + `Ascend(thread)` (AE 47496 / 47484, SE 46240 / 46229, verified
  function rows). **What it writes, stated:** through those two engine functions only,
  the thread's `state` (+0x148, to failed if not already interrupted), `cur_node`
  (+0x138, to the node's parent) and `phase` (+0x14C, to 0). It pushes and pops nothing:
  the runner then calls the node's OWN pop(), which removes what the node's own act()
  pushed, so the act()/pop() balance above holds by construction. Before calling them
  the seat checks the runner state is exactly "this node is `cur_node`, phase 1"; any
  other state passes through to the original update and is counted. Never a raw write
  of those fields, never SetFailed alone (a failed-but-not-ascended node would be
  updated again next step). The act() half of the same leash is the unchanged ForceFail
  pair; the leash arms only when BOTH halves install on all 14 leaves. The runner's `control`
  and the TLS thread the engine's own leaves use are the same object (proof cited in
  `core/CombatBehaviorRE.h`). (The
  2026-09-03 T1 probe crash that made "hand-rolled SetFailed" a warning called 0x5572A0
  = id 33171, the data-stack PUSH, believing it was SetFailed — `core/CombatBehaviorRE.h`
  records the correction.)
- **A path you cannot yet close is a DOCUMENTED GAP, never a silent one.** If an
  enumerated path has no clean, RTTI-verified (#17), version-robust seat on the
  pinned CommonLib, DO NOT hook a blind slot and DO NOT pretend the facet is
  complete. Record it in the audit as an open gap with the precise RE it needs, and
  scope the claim so a client is not handed a facet whose competitors APMF cannot
  actually silence. A partial deny presented as complete is the failure this rule
  exists to end.
- **Bounded by #0.** Deny completeness NEVER licenses manufacturing behavior to
  "win" — the deny still only ever flips the engine's own YES to NO for an actor
  APMF holds a claim on (#17 engine-answer-first). Completeness is about covering
  every SUPPRESSION path, never about adding a drive/re-assert to out-muscle a
  competitor.
