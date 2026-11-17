# Where the engine ends a fight: task-queue message 0x2B (2026-11-16, disassembly of all three builds)

Written for the v0.20.5 hotfix (`fix/stopcombat-main`). Harbinger 0.20.4 (stability H1, merge `ed97853`) ran
`Actor::StopCombat` inside the actor's own `Character::UpdateCombat` job and the Deck hung at the first combat
frame after a load. This note records what the three executables say about where the ENGINE runs StopCombat when
a fight ends, and what Harbinger now does with it (`native/core/CombatOps.{h,cpp}`, INVARIANTS #4a).

Sources: `binaries/1.6.1170/SkyrimSE.unpacked.exe` (AE), `binaries/1.5.97/SkyrimSE.unpacked.exe` (SE),
`binaries/1.7.104/SkyrimSE.exe`, `objdump -d -M intel`, the Address Library (`versionlib-1-6-1170-0.bin`,
`version-1-5-97-0.bin`), the fork's 1.7.104 id map. RVAs below, VA = 0x140000000 + RVA.

## 1. The field failure (evidence: MFO `_evidence/deck-load-1116/`)

- 19:31:15.816 `[substrate] 0x750012C6 (Jesper) came out of the save in combat with the anchor ... StopCombat queued
  on its own combat update` (the load's anchor clean, `CombatSubstrate.cpp` AfterLoad).
- 19:31:19.980 the first combat frame of the session (`CombatAreaStandard::Update ran on a combat thread`). It is the
  last line of `APMF.log`. MFO's frame-driven lines stop at 19:31:19.974. Only MFO's sleeper thread keeps logging
  (`worker ticks=34 main drains=0`) for 55 s. No crash log. The process was alive.
- The one new thing on that frame: `combatops::DrainFor` in `E4Outer` (`TargetPin.cpp`) ran `Actor::StopCombat` for
  Jesper on a BSJobs worker inside the "Combat and magic" job list.

The primitive inside the engine that stalled is NOT proven (no stack was captured). What is proven is below: the
engine never runs StopCombat at that point, and 0.20.4's header said it did.

## 2. UpdateCombat does not end the fight. It posts a message.

`Character::UpdateCombat` (slot 0xE4): AE 38565 `0x6B6E70`, SE 37612 `0x625700`, 1.7.104 `0x6C98F0`. Its tail, AE:

```
6b7064  test rsi,rsi                 ; the controller
6b7067  je   6b7078
6b7069  mov  rdx,rdi                 ; the actor
6b706c  mov  rcx,[rip+...]           ; 0x31993A0 = TaskQueueInterface singleton (AE id 403759)
6b7073  call 0x656530                ; the sender, below
6b7078  mov  rcx,rdi / call 0x662be0
```

reached when the actor is dead (`call [vtbl+0x4C8]`), has no process, or the controller says the fight is over
(`0x5594C0` returned true, `bpl`). SE twin: `0x6258F7 mov rcx,[0x2F38978]` (SE id 517228), `0x6258FE call 0x5C57A0`.
1.7.104 twin: `0x6C9AEC mov rcx,[0x3242650]`, `0x6C9AF3 call 0x668F20`.

**The sender** `void(TaskQueueInterface* this, Actor* actor)`: AE id 36960 `0x656530`, SE id 35985 `0x5C57A0`,
1.7.104 `0x668F20` (no id in the fork's table; the first 35 bytes of the AE body hit it alone, and it has the same
two callers). AE body, whole:

```
656530  push rbx / sub rsp,0xa0
656539  mov  rax,rdx
65653c  mov  dword [rsp+0x20],0x2b   ; message type 0x2B
656544  mov  rbx,rcx
656547  lea  rdx,[rsp+0xc0] / mov rcx,rax / call 0x2efac0   ; the actor's handle
65655a  mov  edx,[rax] / mov [rsp+0x28],edx                 ; handle at +8
656560  lea  rdx,[rsp+0x20] / mov rcx,rbx / call 0x6595d0   ; the poster
```

Its second caller on each build: AE `0x664BFB` (in `0x664950`), SE `0x5D4345`, 1.7.104 `0x67761B` (in `0x677370`).

**The poster** AE id 36993 `0x6595D0` (1.7.104 `0x66BFC0`; inlined into the sender on SE):

```
6595f8  cmp  byte [0x20130B8],0      ; the queue flag (AE id 381120; SE 0x1DEF8A0 id 509007)
6595ff  je   6596e2                  ; flag clear -> run it now
...                                  ; spinlock 0x31993D8 / 0x31993DC
659695  mov  rcx,[rbp+0x10] / mov rdx,rsi / call [vtbl+8]    ; push onto the current queue
6596de  test bl,bl / jne 6596ea      ; pushed -> return
6596e2  mov  rcx,rsi / call 0x656f30 ; NOT pushed, or flag clear: run the message on THIS thread
```

The flag is 1 in the image's `.data`. Its only writers: AE 13754 `0x1B9AB0` (saves it, clears it, puts it back; called
from `0x19A81F` and `0x6491E4`) and AE 36551 `0x641C60` (clears it). So in an ordinary frame the message is QUEUED.

**The message processor** AE id 36991 `0x656F30` is a jump table on the type (table at `0x65940C`). Type 0x2B:

```
657fee  mov  eax,[rdi+8]             ; the handle
658003  call 0x261e40                ; handle -> NiPointer<Actor> (holds a reference)
658010  je   65801f
658012  mov  rax,[rbx] / mov rcx,rbx
658018  call [rax+0x728]             ; Actor::StopCombat (slot 0xE5)
65801f  ...                          ; release the reference
```

## 3. Where the queue is drained

The drain: AE id 36891 `0x654880`, SE id 35916 `0x5C1BE0`, 1.7.104 `0x667270`. It swaps the queue the posters push to
(under the spinlock), pops every message of the other one and runs each through the processor. Exactly two call
sites on each build (E8 scan of `.text`):

| | in Main::Update | in the wrapper the "Post process" job calls |
|---|---|---|
| AE | `0x646A3C` (Main::Update 36564 `0x645EA0` +0xB9C) | `0x6479FC` (36588 `0x6479E0` +0x1C), called at `0x6D356A` from Post process 39092 `0x6D34B0` |
| SE | `0x5B36A8` (35565 `0x5B2FF0` +0x6B8) | `0x5B44DC` (35582 `0x5B44C0` +0x1C), called at `0x640E62` from Post process 38136 `0x640DB0` |
| 1.7.104 | `0x65941E` (`0x658870` +0xBAE) | `0x65A3DC` (`0x65A3C0` +0x1C), called at `0x6E610A` from Post process `0x6E6050` |

**The Main::Update site is the PAUSED road.** AE `0x645F69..0x645F8F`: `r15b = 1` when the UI's pause count
(`[UI+0x160]`) is 0 and `Main+0x16` is 0, that is, the game is running. `0x6469C2 test r15b,r15b / 0x6469C5 jne
0x646A8F` jumps over the drain at `0x646A3C` when it is. SE: `0x5B364D test dil,dil / jne 0x5B36FC`. 1.7.104:
`0x6593A4 test r15b,r15b / jne 0x659471`. That site sits after the frame's AI job pass was joined: `0x64694D call
0x5FC210` waits on the pass's done event (`[0x31771D0]+0x60`) when a pass was kicked (`0x64613F call 0x5FC180`: reset
the 13 job lists, `SetEvent([+0x58])`).

**The running game drains in the "Post process" frame job.** The AE frame job table (`.data 0x2011920`, entries
`{fn, name}`), in order, with the list ends:

```
TES animation, Water audio, Texture update, Combat manager, Partial Terrain Manager Update, JobListEnd,
Actor update, Wait Player Update 1, JobListEnd,
Actor update end, Actor movement, ..., Detection, ..., Ragdoll animation, VM update, Ragdoll signal/sync,
  ClonePools Update, Combat and magic (0x6D2920), Destructible, JobListEnd,
Quest for events, ..., Process lists, ..., VM render-safe, JobListEnd,
Garbage, ..., Pathing and nav-mesh, JobListEnd,
Non render-safe AI, Anim SG signal/sync, Animated cell object update, Cell update signal/sync,
  Post process (0x6D34B0), Poll controls, JobListEnd,
Face morphing, Sky, Shared particles, Update grass, End signal/sync, Post reset
```

"Combat and magic" (the CombatManager walker `0x83D070` pushes one `UpdateCombat` job per member of every combat
group, submits the list and waits for it) is three lists before "Post process". So a fight that ends inside an
actor's UpdateCombat is torn down later in the frame, outside every combat job, by whichever thread runs "Post
process". NOT PROVEN here: that the job graph runs its lists strictly one after another (the executor was not traced;
the same open point as MFO ENGINE_NOTES 0.47). What IS proven is the engine's own choice: it defers its own combat
end out of the combat job to this drain.

**SKSE rides the same two call sites.** `skse64/Hooks_Threads.cpp` (1.6.1170): `ProcessTasks_HookTarget_Enter(0x00645EA0
+ 0xB9C)` and `Enter2(0x006479E0 + 0x1C)`; `BSTaskPool::ProcessTasks` calls the original drain, then runs the SKSE
task queue. That is why an SKSE `AddTask` body runs on a job worker while the game runs (MFO ENGINE_NOTES 0.30) and on
main while it is paused.

## 4. What 0.20.4 got wrong

`core/CombatOps.h` (0.20.4) said the engine "frees controllers from the same job lists itself" and cited "the
UpdateCombat tail's message processor 0x656F30 calls it at 0x658018". The processor does call StopCombat at
`0x658018`, but it is reached from the DRAIN, not from the tail. The tail only posts. The inline branch of the poster
(`0x6596E2`) exists, and is taken only with the queue flag clear or a refused push. The header's other citation (AE 37347 calls
StopCombat inside the per-actor "Actor update" job at `0x667BA2`, a different job list, before that actor's own
Actor::Update) was not re-examined here and nothing is built on it any more.

## 5. What Harbinger does now (v0.20.5)

`combatops::StopCombat` (main) calls the engine's sender with the TaskQueueInterface singleton and the actor. The
engine queues its own message 0x2B and runs `Actor::StopCombat` at its own drain. Harbinger calls `Actor::StopCombat`
itself only inside a save (the engine writes the actor records right after, and the save seats run with the job
graph idle). Nothing of Harbinger runs StopCombat on a combat job. Rows: `CombatOps.TaskQueue.QueueStopCombat` (AE
36960, SE 35985, 1.7.104 raw `0x668F20`) in `native/VerifiedAddresses.h`; the singleton is CommonLib's
`TaskQueueInterface::GetSingleton()` (SE 517228 / AE 403759, 1.7.104 `0x3242650` in the fork's table, the same global
the three UpdateCombat tails load).

Roads weighed:

- **(a) the engine's message through the engine's sender: CHOSEN.** One call, one verified row, no hook. The free has
  the engine's timing by construction, and it is the path every natural combat end already takes.
- **(b) a Harbinger seat at the drain.** Not needed: SKSE's task queue already runs at exactly those two call sites,
  so a seat there would be an SKSE `AddTask`. It would keep the controller identity check at the free. It was not
  chosen because it puts a Harbinger frame under the free again, at a point whose history in this family is mixed
  (MFO moved its own StopCombats off the SKSE task worker after the v1.0.53 / v1.0.58 deck freezes), and (a) is the
  engine's own code end to end.
- **(c) the 0.20.3 road (StopCombat from Harbinger's main pump).** Kept only as the flagged fallback when the sender's
  row is refused. It re-opens APMF-B26.

Lost against 0.20.4, flagged: the message names the actor, not the controller. See `core/CombatOps.h` "WHAT WAS LOST".
