# Ghost playback, time control and spectate entry (USA CUSA13795 1.21)

ELF addresses of `CTRNF/out/eboot.elf` (patch XML = ELF + 0x400000). Read from code only; the game was not run.
**V** = verified in the disassembly. **I** = inferred (read from how the code is used, not proven, not tested).
Older notes: `~/Desktop/CTRNF-GhostShare/notes/ghost-code.md` (classes, spawn), `ghost-menu*.md`; `~/Desktop/CTRNF-Mods/notes/time-trial.md`.

## 1. Ghost playback clock

CGhostManager (`[0x2abc3d0]`): +0x0c `_isRecording`, +0x10 `_recordTime`, +0x14 `_isPlaying`, +0x18 `_playTime`, +0x40 `_playingGhosts`.
CGhost: +0x10 `_kart` handle, +0x30 move keys, +0x38 snapshot (input) keys, +0x40 `_playMoveKeyIndex`, +0x44 `_playSnapshotKeyIndex`.

| ELF | What | |
|---|---|---|
| 0xea2d90 | race tick (event handler, registered at 0xe9b1c5). Only when `[[0x2acde48]+0xd4c] == 2` (Time Trial) it calls 0xfce570 at **0xea2e63** | V (tick = every frame: I) |
| 0xfce570 | `(mgr)`: returns if `_isPlaying == 0`. `dt = [F+0x28] ? 0 : [F+0x24]` (F = `[0x2ab4100]`), **0xfce5a2** `_playTime += dt`, then for each ghost in `_playingGhosts` with a live `_kart`: 0xfce6a0(mgr, ghost), 0xfcea00(mgr, ghost) | V |
| 0xfce6a0 | move keys: from index i = +0x40, finds the pair with `key[i].t <= playTime < key[i+1].t` (t = u16/100.0), stores i, lerps position (0x3dc8e0) and angles (0xbabbf0) | V |
| 0xfcea00 | snapshot keys: same search from +0x44; stores the index of the key now in effect | V |
| 0xfce250 | `(mgr, kartEntity)` -> current snapshot key, or null if +0x44 < 0. Used by 0xe54c20 (ghost kart input) and 0xe2f9a0+0x7f8 | V |
| 0xfce3b0 | `(mgr)`: 0xfce410 for every playing ghost. Called once at **0xea6381** in 0xea55c0 (race start handler, registered at 0xe9b2f1) | V |
| 0xfce410 | `(mgr, ghost)`: `mgr._playTime = 0`, ghost +0x40/+0x44 = 0 (one qword), kart to key 0 (position + angles), `_isPlaying = 1` (0xfce550). Needs >= 1 move key and a live `_kart` | V |
| 0xea6390 | right after: `call 0xfcb9a0(mgr, r14)` = start recording the player | V |

Answers:
- **Forward only (V).** Both searches start at the stored index and only go up. If `playTime` is lower than `key[index].t`
  (rewind), no pair matches: the move index runs to `count-1` and the kart stops updating for good; the snapshot index
  goes to `count-1`, next frame to **-1** and stays -1 (0xfcea00 returns at once on -1). **A rewind must reset +0x40/+0x44**
  (0, or the right index found by the patch).
- **Index 0 quirk (V):** when the match is pair 0 (`key0.t <= t < key1.t`), 0xfce6a0 stores 0 but does **not** move the kart
  (`test eax,eax; jle` at 0xfce7a6). Key 0 to key 1 can be up to 3.8 s in the countdown. When rewinding to `t < key1.t`, call
  0xfce410(mgr, ghost) to put the kart on key 0.
- **Past the last key (V):** move index = `count-1`, no more transform updates: the kart stays at its last drawn pose (a
  frame before the last key). Snapshot index -> -1, 0xfce250 returns null, 0xe54c20 takes its "no key" path (neutral input: I).
  Nothing in the ghost code hides or removes the kart; `_isPlaying` is only cleared at level exit (0xf9b477). So the ghost
  does not disappear (I for the whole game: other systems not traced).
- **Clock source (V):** frame dt, not the race clock. `dt = [0x2ab4100]+0x24` (`_gameFrameTime`, already scaled by game speed),
  0 while `+0x28` (`_gamePaused`) is set. All ghosts share one `_playTime`; it starts at 0 in 0xea55c0 (start of the countdown, I:
  the `raceStart` input bit is at ~3.80 s in all files).
- **Setting `_playTime` each frame:** yes, at **0xfce5a2** (hook below). The value we return is stored by the game's own
  `vmovss [r14+0x18], xmm0` at 0xfce5a8 and used by both searches in the same frame, so there is no race with `+= dt`.
  0xfce570 runs every frame while paused too (it checks `+0x28` itself) (I).

## 2. Race clock, finish, saving

| ELF | What | |
|---|---|---|
| 0xf34fd0 | `(this, racer, lap)`: lap counter; when `lap - 1 == [[0x2ad4e30]+0xc]` (lap count) -> 0xf350c0 and **0xf351b0** (unless `racer+0x8c`). Callers 0xf343a5, 0xf9f540 (checkpoint/lap code, I) | V |
| 0xd29fd0 | calls 0xf351b0 for every racer of a list (force finish; trigger not traced) | V / trigger I |
| 0xf351b0 | racer finished: 0xfcefe0 (ghost race end) then 0xf353b0 or 0xf35350, then 0xff32e0 (results/records, not traced) | V |
| 0xfcefe0 | **returns at once if `mgr+0x0c` (`_isRecording`) == 0** (0xfceff6); else 0xfcc430 (save ghost, unlocks, trophies) | V |
| 0xfce000 | recording tick: `_recordTime += dt`; stops itself at 360.0 s (`+0x0c = 0` at 0xfce073) | V |
| 0xf9b450 | level exit: `_isRecording = 0`, `_isPlaying = 0`, clears lists (does not save) | V |

- What ends a Time Trial: the **player's** kart completing the last lap (0xf34fd0 -> 0xf351b0). Ghosts are not stopped by it (V);
  they only stop at their last key.
- Spectate never finishes as long as the player kart never crosses the line: no results screen, no ghost save, no unlocks, no
  trophies, no stats (all of these sit behind 0xf351b0 -> 0xfcefe0 -> 0xfcc430) (V for the ghost/unlock part; leaderboard and
  "best time" writes in 0xf353b0/0xff32e0 not traced, I).
- Extra guard: after 0xfcb9a0 at 0xea6390, set `mgr+0x0c = 0`. Then even a forced finish (0xd29fd0) saves no ghost and gives
  no unlock (V for 0xfcefe0). The player's empty recording is cleared at level exit.
- Race clock HUD: `CRaceClockComponent` (meta string 0x1d402e3): +0x28 `_raceTimer`, +0x30 `_freezeTimer`, +0x38 `_freezeDuration`,
  +0x3c `_countdownTimeLeft`, +0x40 `_sentLowTimeLeftMessage`, +0x48 `_raceClockData` (`CRaceClockData`: `_raceClockMode`
  ERCM_CountUp/CountDown, `_timeToCountDown`, `_lowTimeLeftThreshold`) (offsets table 0x1da948a, V). HUD reads it via
  `getRaceClockTime_VSC` 0x1193810 -> 0x10556b0 (float from a clock component on a global entity) (I). It counts up from GO with
  game time and knows nothing about the ghost clock; it will be wrong after a rewind. `hideClock_VSC` exists. Not traced further.
- AFK kick (`CAFKChallenger`, `eNCV_AFKKick`) looks online-only (I).

## 3. Global time: pause and speed

Frame-time singleton `[0x2ab4100]` (vtable 0x254c9a8, set by 0xa318d0; fields from the reflection table 0x1d9fbb0, V):

| Off | Field | Notes |
|---|---|---|
| +0x0c | _lastTime | ticks |
| +0x14 / +0x18 | _systemTime / **_systemFrameTime** | real dt in s, never scaled |
| +0x20 / +0x24 | _gameTime / **_gameFrameTime** | `clamp(sysdt, 0.001, 0.1) * _gameSpeed`; not updated while paused (0xa31a10) |
| +0x28 | **_gamePaused** | readers use 0 dt when set; set e.g. at 0xad55b7 |
| +0x2c | **_gameSpeed** | float, default 1.0 |
| +0x30 / +0x31 | _drawGameSpeed / _gameFrameTimeWasScaled | debug text / speed != 1 |
| +0x34 | _frameStep | fixed step 1/30 option |

- `setGameSpeed_VSC` 0x10ddcf0 writes +0x2c only if `0 < x <= 1.0` (0x1cab828). `getGameSpeed_VSC` 0x10ddd30. A debug slider
  (0xb9c440) also writes it. Nothing else found that resets it (0xb878f0 sets 1.0, no direct caller) (V/I).
- `pauseGame_VSC` 0x10ddbb0 -> 0x1035e70(reason, bool) -> pause stack 0x98b730; `unpauseGame_VSC` -> 0x1035ed0 -> 0x98a620 (V). Do not
  write +0x28 by hand: it is the game's own pause state (pause menu) (I).
- Something divides by `_gameSpeed` (0x101ca07): **never set it to 0** (V).
- Other time scales: `CActorTimeScale` / `_timeScaleList` / `_freezeFrameTimeScale` (per-actor freeze frames), not needed.

Answers:
- **Pause (x0):** freeze only the ghost clock (keep returning the same `_playTime`). The game keeps running: camera, HUD,
  sounds, particles. The frozen ghost keeps its last snapshot input (e.g. accelerate held), so engine sound/wheel animation may
  go on (I). Safest; no game state is touched. A full freeze via `_gamePaused` or speed 0 is not safe (see above).
- **Speeds:** the patch's own clock gives exact ghost motion for every speed and for reverse. The rest of the world (smoke,
  sparks, driver animation) runs at x1 unless `_gameSpeed` is also set. Setting `_gameSpeed` to 0.1/0.5 is the game's own
  path (VSC range). 2-4 is outside the VSC range: untested (physics of the idle player kart with dt up to 0.4 s, effects).
  Audio pitch with game speed: unknown. If `_gameSpeed` is set, drive the ghost clock from `_systemFrameTime` x speed (not
  `_gameFrameTime`), or speed is applied twice. Restore 1.0 when spectate ends.

## 4. Ghost select menu: a Spectate button

Facts (V unless marked):
- In this menu **Square = Option1 (10) = "CONTINUE"** (Root VSC, ProjectInputPressed), Triangle = Option2 (11) = Options
  (GhostShare build.py, tested in game; ghost-menu-prompts.md). **Square is taken.** Free on an entry: R1 16 is GhostShare's
  export (own ghost only); L1 17, L2 15, R2 14 have no known use (probe them in game, I).
- Ghost entry event handler 0x9b3830: r15 = entry behavior, r13 = event, `[rbp-0xa8]` = entry placeable,
  `project = [placeable+0x20]`. Dispatch compares (each 9-12 bytes, only the first byte is a jump target, checked):

| ELF | Bytes | Instruction | Use |
|---|---|---|---|
| **0x9b38ae** | `48 39 d8 0f 84 9c 01 00 00` | `cmp rax,rbx; je 0x9b3a53` (FocusInputPressed?) | Spectate button. Next instr 0x9b38b7 |
| 0x9b38da | `48 39 d8 0f 84 4d 02 00 00` | `cmp rax,rbx; je 0x9b3b30` (PlaceableGainFocus?) | add legend "SPECTATE" |
| 0x9b3915 | `4c 39 f8 4c 89 e3 0f 85 13 01 00 00` | `cmp rax,r15; mov rbx,r12; jne 0x9b3a34` (LoseFocus?) | remove legend |

  These run before GhostShare's sites (0x9b3a53, 0x9b3baa, 0x9b3998) and do not overlap them. Exit = 0x9b3a34.
- Selection state lives in the ghost manager: player ghost `gm+0x48`, challenge (and GhostShare shared) headers in the list
  `gm+0x50` (count +0xc). GhostShare takes its own headers out of that list only at race start (0xf9b298), so
  `gm+0x48 || [gm+0x50]+0xc > 0` = "something selected" with or without GhostShare (V from GhostShare source).
- Continue button (0x127ec50, signal 8 path 0x127ee57): `0xc2e400([0x2ad4e30], 0)`; `ev = 0x1255f90(pool)` (event meta slot
  0x2ae67b0, I = CGuiEventOctaneGhostContinueButtonPressed); `ev+0x18 = 1`; `project->vtable[0xb0](project, ev)` with
  `project = [placeable+0x20]`; `0x149d260(ev)`. The Root VSC then starts the race (ghost-menu.md). Pool 0 for 0x1255f90: I.
- Race start then runs as normal: 0xf9b250 (level start: spawns the selected ghosts; GhostShare adds shared ones), 0xea55c0
  (countdown: playback reset, recording start).
- Legend: `0xa09d30(project, button, &igString, 1)`; remove via `CGuiManager([0x2ac0dc0])+0xf0` event, `+0x18 = button`,
  `project->vtable[0xb0]` (ghost-menu-prompts.md). Keys: 5 L1, 6 R1 (GhostShare), 7 L2, 8 R2.

## Addresses already written by other mods

| Mod (file) | ELF start | Bytes | What |
|---|---|---|---|
| GhostShare (`patches/GhostShare.xml`) | 0x9e2756 | 5 | call -> fill ghost list |
| | 0x9e2d22 | 5 | same |
| | 0x9b3e88 | 5 | entry setup event |
| | 0x9b3a53 | 7 | entry button (`cmp signal,8; jne`) |
| | 0x9b3baa | 5 | entry focus gain |
| | 0x9b3998 | 5 | entry focus lost |
| | 0xf9b298 | 5 | race start, take chosen |
| | 0xf9b329 | 6 | race start, add chosen |
| | 0x2320000-0x23239ca | code | reserved to 0x2338000 |
| | 0x2bee680-0x2beef80 | state (not written by XML) | real size ~0x350 (from source: I) |
| TimeTrialBeatAll | 0xfcc890 | 23 | race end unlock |
| | 0x2338000-0x23381f2 | code | |
| ReservesMeter | 0xe75240 | 10 | |
| | 0xe5aa30 | 6 | |
| | 0x2339000-0x233a1e0 | code | reserved to 0x233c078 |
| | 0x2beef80-0x2bef000 | state (not written) | |
| tomtotom drift gauge (`mods/`, may be installed by users) | 0x112eb08, 0x112ec33 | 5 each | |
| | 0x2694a0 | 8 | |
| | 0x2317820-0x23192df | code | |
| far-camera (`mods/`) | 0xc89111 | 1 | |
| skip-startup-videos (`mods/`) | 0x9f7843 | 2 | |

Free space:
- Code (R+X, ELF zero padding 0x2317810-0x233c078): **0x23192e0-0x2320000 (27 KB) is free and not reserved**;
  0x23239ca-0x2338000 is free but inside GhostShare's reservation; 0x23381f2-0x2339000 (3.5 KB) free.
- **Writable state: none left.** The only RW free space (0x2bee66d-0x2bef000, the page after the game's data; the port maps
  4 KB pages, `src/probe.c` 1190-1198) is fully reserved by GhostShare + Reserves. Best fix: shrink GhostShare's reservation
  (its `struct state` is ~0x350 bytes; only its build.py check and `_Static_assert` change, the published binary stays the
  same) and give Spectate e.g. 0x2beea80-0x2beef80 (0x500). Needs the owner's OK. Do not use the 40 bytes after the relro
  segment (0x2808fd8-0x2809000): mapped RW by the port but not proven free.

## Plan

Hooks (none overlaps the table above; bytes verified; jump targets checked):
1. **0xfce5a2** `c4 c1 7a 58 46 18` (`vaddss xmm0, xmm0, [r14+0x18]`) -> `call hook_clock; nop`. Stub: `mov rdi, r14; jmp sp_clock`,
   C: `float sp_clock(void *mgr, float game_dt)`; return `old + game_dt` when not spectating. rsp is 16-aligned at the site, so
   the stub sees a normal function entry. Caller-saved registers are free (all reloaded after 0xfce5a8, V).
   Spectating:
   ```c
   F = [0x2ab4100]; dt = F->paused(+0x28) ? 0 : clamp(F->sysdt(+0x18), 0.001, 0.1);
   t = sp.time + dt * sp.speed * sp.dir;          // dir +1 / -1 (D-pad hold), speed 0 = pause
   t = clamp(t, 0, sp.end);                       // end = last move key time (u16/100) of the watched ghost or the max
   if (t < old) for each ghost g in mgr->_playingGhosts with a live _kart:
       g->moveIdx = 0; g->snapIdx = 0;            // one qword store, like 0xfce410
       if (t < key1(g).t) 0xfce410(mgr, g);       // put the kart on key 0 (it also writes mgr+0x18 = 0: harmless)
   sp.time = t; return t;                         // the game stores it and plays all ghosts at t
   ```
   Faster rewind: binary-search the right index instead of 0 (keys are sorted).
2. **0xea6390** `e8 0b 56 12 00` (`call 0xfcb9a0`, start recording) -> `call sp_race_start(mgr, kart)`: call 0xfcb9a0, then if
   `sp.armed`: `sp.active = 1`, `sp.time = 0`, `sp.speed = 1`, compute `sp.end`, `mgr+0x0c = 0` (no save, no unlock).
3. **0x9b38ae** (9 bytes) -> `jmp hook_menu; 4 x nop`:
   ```asm
   hook_menu: cmp rax, rbx ; jne 0x9b38b7          ; not FocusInputPressed: game code
              cmp dword ptr [r13+0x18], SPECTATE_SIGNAL ; jne 0x9b3a53   ; game (or GhostShare) button code
              SAVE ; mov rdi, r15 ; mov rsi, [rbp-0xa8] ; call sp_menu_button ; RESTORE ; jmp 0x9b3a34
   ```
   `sp_menu_button`: if nothing selected, do nothing; else `sp.armed = 1` and send the Continue event as 0x127ee57 does.
4. Optional legend: 0x9b38da / 0x9b3915 (same pattern) with 0xa09d30 and the remove event.
5. Clear `sp.armed`/`sp.active` when spectate ends: when `_isPlaying` is 0 at our next race start without `armed`, or with a hook
   at level exit 0xf9b472 (`41 c6 46 0c 00`, not checked for jump targets yet). Restore `_gameSpeed = 1.0` if the patch set it.

Game calls used: 0xfce410, 0x149fca0 (slow handle path, if `_kart` is read by the patch), 0xc2e400, 0x1255f90, 0x149d260,
0xa09d30, 0x1574080 / 0x1518e30 (strings).

Open risks:
- **Square is already Continue** in this menu; the owner must pick another button (L1/L2/R2) or accept that Square on a ghost
  entry also fires the Root's Continue (event order unknown).
- **No writable memory left** for the patch state (see Free space): needs an agreed split with GhostShare.
- Player kart: still spawned, on the start line, takes input, and the camera follows it. Blocking input, hiding the kart and
  moving the camera to the watched ghost are not researched yet. If the player kart can still drive and finish, the race ends.
- Ghost karts and lap/checkpoint logic: if ghost karts have lap trackers, a rewind across the line or checkpoints may confuse
  them (wrong way, extra laps). Not traced. Teleporting karts back may also glitch trails, motion blur or sounds.
- HUD race clock follows game time, not the ghost clock; it needs hiding or replacing.
- Emulating Continue from an entry: pool 0 and the Root VSC reaction are not tested.
- Retry from the pause menu: whether 0xf9b250/0xea55c0 run again (and so whether spectate stays on) is unknown.
- `_gameSpeed` above 1.0 and audio behaviour with game speed: untested.
- Max 8 racers: player kart + up to 7 ghosts (0xf9bee0 refuses more).
