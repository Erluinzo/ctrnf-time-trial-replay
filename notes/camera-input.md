# Camera, input and HUD map for the spectator mode (USA CUSA13795 1.21)

ELF addresses of `CTRNF/out/eboot.elf` (patch XML = ELF + 0x400000). Read from code only; nothing was run (2026-10-29).
**V** = verified in the disassembly. **I** = inferred (from how the code uses it; test in game).
Class sizes/field offsets come from the reflection tables (every `0x15083e0` class registration and its `0x1507980`
field table); they are V unless marked. Ghost classes and managers: see `CTRNF-GhostShare/notes/ghost-code.md`.

## Engine helpers used below
| ELF | Call | |
|---|---|---|
| 0x14bea20 | `makeHandle(void **slot, obj)`: releases the old handle in `*slot`, stores a new one to `obj` (obj 0 = clear) | V |
| 0x149fca0 | `resolveHandle(void **slot)` slow path (handle flags 0x10000000) | V (ghost notes) |
| 0x3dbf90 | `findComponent(entity, meta, 0)`, meta = value of a meta slot (`*(void**)slot`) | V |
| 0x149d260 | `release(obj)` | V |
| 0xc02600 | `CPlayer *findPlayer(playerList, playerId)` (compares `CPlayer._playerId`) | V |

Handle object = `[h+0x28]` when `[h] & 0x30000000 == 0` (else see the Reserves `handle_object` helper).

## 1. Race camera
### Classes
| Class (size, meta slot) | Fields used |
|---|---|
| CCameraSystem (0x58, 0x2ab24d0); instance pointer **[0x2ab2538]** | +0x18 `_cameraInstancesHashTable`, +0x20 `_cameraInstancesList` |
| CCameraSystemInstance (0x3d0) | +0x58 `_activeCameraBoxes`, +0x70 `_ownerPlayer`, +0x292 `_isGameplayCameraPaused` |
| CCameraBase (0x130) | +0x11d `_resetOnNextUpdate`, +0x11e `_isActivated`, +0x11f `_hasListener`, +0x120 `_playerId` |
| **CKartCamera** (0x290, meta slot **0x2ac42e8**, vptr 0x256eb38) | +0x1d0 `_focusedEntity` (handle), +0x1d8 `_cameraUpdateActive`, +0x248 `_kartControllerComponentHandle`, +0x250 `_currentMode` (near/far), +0x254 `_isReverse`, +0x278 **`_overrideFocusedEntity`** (handle) |
| CStackCamera (0x1f0, 0x2ad8980) | +0x1cc `_playerId`, +0x1d0 `_currentCameraControllers` (used by online spectate only) |

### How the kart camera picks its kart (V)
| ELF | What |
|---|---|
| 0xc4b110 | `CCameraSystemInstance *()` for the first local player (type `[player+0x20] <= 7`) |
| 0xc4b230 | `camera *(list)`: first active camera box -> its camera (`[box+0x168]` handle, else `[box+0x160]`) |
| 0xc890c0 | **CKartCamera update** (rdi = camera, xmm0 = dt): `_focusedEntity` -> 0xc0fa60 (proxy -> real entity, 0 = keep) -> `CKartControllerComponent` (0x3dbf90, meta 0x2ac4580) -> `_kartControllerComponentHandle`. If `_focusedEntity` is dead/null it calls 0xc8ade0 first (0xc8a542). It sets `actor+0x14c = 1` and calls the actor's vtable+0x128 if that byte was not 1 (0xc8a763) |
| **0xc8ade0** | **refresh focus** (rdi = camera): `_overrideFocusedEntity` set -> focus = that entity; else player = `0xc02600([[0x2ad1160]+0x10], cam._playerId)`, entity = `player._hero` (+0x10) -> 0xc0fa60(`[0x2ab99b8]`, e) or e. Writes `_focusedEntity`. Called by 0xc88960 (activate) and 0xc890c0 |
| 0xc88b80 | camera input (look back / near-far): kart from `[0x2ad0730 + id*0x78 + 0x50]` -> 0xfd3d80, controller of `cam._playerId`; if `ActorInput._lockControls->vtable+0xb0()` (is locked) is true it skips the buttons (0xc88c56) |
| 0xe17811 | race code gets the active camera the same way and checks `vtable[0]() == [0x2ac42e8]` (is CKartCamera): model for the patch |

Nothing in the game writes `_overrideFocusedEntity` (only the dtor 0xc8baf0 clears it): it is free for us (I: maybe set by data/VSC elsewhere, not seen).

### Retarget recipe (V calls, I result)
```c
void *inst = fn_0xc4b110();
void *cam  = inst ? fn_0xc4b230(*(void **)((char *)inst + 0x58)) : 0;
if (cam && (*(void *(**)(void *))*(void **)cam)(cam) == *(void **)0x2ac42e8) {  /* CKartCamera */
    void *kart = handle_object((void **)((char *)ghost + 0x10));                /* CGhost._kart */
    fn_0x14bea20((char *)cam + 0x278, kart);   /* _overrideFocusedEntity = ghost kart */
    fn_0xc8ade0(cam);                          /* _focusedEntity follows now, not next frame */
    *((char *)cam + 0x11d) = 1;                /* optional: snap instead of swoop (I) */
}
/* end of spectating: fn_0x14bea20((char *)cam + 0x278, 0); fn_0xc8ade0(cam); */
```
I: the camera's speed/boost/drift offsets read the ghost's `CKartControllerComponent`; ghosts are moved kinematically, so
these offsets may stay flat (camera feels stiffer). Side effect (V): the camera sets `ghostActor+0x14c = 1`
(`mIsPlayerFastFlagHack`) and calls its vtable+0x128.

### Online spectator (the game's own model, V)
CMultiplayerSpectatorComponent (0x38, meta 0x2ae4380): +0x28 `_hud`, +0x30 `_playerIndex`, +0x34 `_hasSetSpectatorCamera`.
Only for players with `CPlayer._isSpectator` (+0xc4) = joined to watch (no own kart). No "spectate after finish" code was found.
| ELF | What |
|---|---|
| 0x1239d10 | init: if first local player is a spectator, opens HUD project "Spectator" (0x986820) into +0x28, registers 0x123a860 |
| 0x123a860 | picks the first remote player (type >= 8), registers the per-frame update 0x123a050 via 0xcdba50 |
| 0x123a050 | per frame: controller of the first local player; `_buttonState & 0x100` (Square) pressed -> next, `& 0x20` (Circle) -> previous (edge = `[ctrl+0xe8] & ~[ctrl+0xf0]`) |
| 0x123a110 | (this, delta): next player with type >= 8 (wraps), 0x123a380, HUD text = `CPlayer._gamertag` (+0x68) |
| 0x123a380 | camera = `0xc4b230(0xc4b750([0x2ab2538], &handle(player))->+0x58)`, needs `_isActivated`; 0xcb8fe0(cam, player, 0) sets `cam._playerId`; kart = `0xe50f70([0x2ac57c0], playerId, 1)`; 0x123a4d0 copies the kart's `CPlayerCameraComponent` controllers into the (stack) camera via 0xc84710 |
It moves the camera by player id, so it does not fit ghosts (no CPlayer). Use `_overrideFocusedEntity` instead.

### Sound
CCameraBase has `_hasListener` (I: listener follows the camera). `CGameSound` (0xe0, meta 0x2abb640): +0x10
`_soundFocusPositionType` (enum 0 Default, 1 Players, 2 CameraPosition, 3 Entity), +0x20 `_soundFocusPositionEntity` (handle).
Open: the `_Instance` address of CGameSound was not found yet; if ghost sounds are quiet, set type 3 + the ghost kart there.

## 2. The player's own kart
| Item | Where | |
|---|---|---|
| Kart entity of a player | `0xe50f70([0x2ac57c0] CKartManager, playerId, 1)`; fallback `[0x2ad0730 + id*0x78 + 0x50]` (as 0x123a46a) | V |
| CActor (0x238) | +0x140 `_player`, +0x148 bits (bits 0-1 `_controlSchemeIndex`), +0x1a0 **`_actorInput`** | V |
| ActorInput (0x60) | +0x10 `_input` (ActorInputCommand*), +0x20 `_previousButtonStates`, +0x48 `_onProcessInputEventList`, +0x58 **`_lockControls`** (lock object; vtable+0xb0 = is locked) | V |
| ActorInputCommand (0x78) | +0x10 `_buttonStates`, +0x18 `_buttonClicks`, +0x20 `_buttonReleases`, +0x68 `_analogStickDeflection` (x, y floats) | V |
| CKartControlSchemeData (0x40) | bit numbers: +0xc accelerate, +0x10 reverse, +0x14 brake, +0x18/+0x1c turn L/R, +0x20 turnAnalog, +0x24 toggleSpeedometer, +0x28 usePowerUp, +0x2c/+0x30 jump/drift 1/2, +0x34 cameraModeToggle, +0x38 cameraReverse | V |

Input path (V): the kart reads its `ActorInput` command each frame (steering 0xf3ef70: `[[actor+0x1a0]+0x10]+0x68`;
ghost karts get theirs from the delegate 0xe54c20 added by 0xf4cf20 with 0xc01b40). The camera reads the raw controller.

### Blocking the kart: lock requests (the race-end code does the same)
| ELF | What | |
|---|---|---|
| 0xc08100 | `lockRequest(void **out, lockObj)`: new request (0xc07ee0) + `0xbc4ee0(req, 1)` | V |
| 0xbc4ee0 | `(req, on)`: calls lock object vtable+0xb8 (add) / +0xc0 (remove), `[req+0x18]` = on | V |
| 0xe7eeb0 | race end: skips ghosts (`_isGhost`), locks `[[actor+0x1a0]+0x58]` and `[actor+0x108]` (`CPhysicalEntity._invulnerable`), keeps requests in its component +0x28/+0x30 | V |
```c
void *lock = 0;
fn_0xc08100(&lock, *(void **)(*(char **)((char *)kart + 0x1a0) + 0x58));   /* lock the player's controls */
/* end: fn_0xbc4ee0(lock, 0); fn_0x149d260(lock); */
```
I: blocks driving, items and the camera buttons (0xc88b80 checks the same lock). Verify in game that the kart stays still.

### Keeping it out of the way
- Not moving -> never finishes -> race end 0xfcefe0/0xfcc430 never runs, nothing is saved; level exit 0xf9b450 drops the recording (V from ghost notes).
- Hide (I, untested): `0x102c8d0(entity, hide=1, 1, 0)` (as race code 0xe1c61f) -> `0x3def00(entity, 0)` (bit 15 of `[e+0x38]`, then vtable+0xc0).
  This looks like **enable/disable**, not only draw: it may stop the kart's components (HUD component, recording). Do not use in v1; the kart
  only shows at the start line (ghosts pass through it).
- `CKartAIComponent._aiLocksPlayerControls` (+0x7a) exists (AI drives the player after the finish); not needed.

## 3. Reading the pad
| Item | Where | |
|---|---|---|
| CInputManager instance | **[0x2ac2e90]**: +0x28 `_controllers` (list: +0xc count, +0x20 data), +0x38 `_controllerMap` int[8] (player id -> controller index), +0x78 `_primaryPlayerId` | V |
| CController (0x2b0) | +0x10 `_inputDevice`, +0x18 `_buttonMap` int[51] (game bit -> device signal, -1 none), **+0xe8 `_buttonState`**, **+0xf0 `_previousButtonState`**, +0xf8 `_forcedButtonState`, +0x100 `_deflections` float[51], +0x1cc thresholds float[51] | V |
| 0xa3b350 | CInputManager update (vtable slot, once per frame I): per controller `prev = cur; cur = forced`; for b < 51: signal value of `_buttonMap[b]` above threshold -> `cur |= 1<<b`, `_deflections[b] = value` | V |
| 0xa3d130 | builds `_buttonMap` from the PS4 table at 0x1d9d480 (30 x {bit, signal, threshold}), then applies remaps from `_inputSystemData` | V |
| Player's controller | `p = first item of [[0x2ad1160]+0x10]` with `[p+0x20] <= 7`; `ctrl = mgr->_controllers->data[mgr->_controllerMap[p->_playerId (+0xb0)]]` (both indexes <= 7), as 0x123a050 | V |

Default PS4 bits (table 0x1d9d480, V): 0/1 left stick X/Y, 2/3 right stick X/Y, 4 Cross, 5 Circle, 6 Triangle, 7 R2, 8 Square,
9 L3, 10 R3, **11 L1**, 12 L2, **13 R1**, **14 D-up, 15 D-down, 16 D-right, 17 D-left**, 18 touchpad, **19 Options**,
20-27 copies (20 Cross, 21 Circle, 22 Square, 23 Triangle, 24 Cross, 25 Square, 26 Triangle, 27 Circle), 48 R2, 49 L2.
Masks: L1 0x800, R1 0x2000, Square 0x100, Triangle 0x40, Up 0x4000, Down 0x8000, Right 0x10000, Left 0x20000, Options 0x80000.
(Device signal enum, 0x24953b0/0x1c35980: 0 PadUp 1 PadDown 2 PadLeft 3 PadRight 4 Circle 5 Square 6 Triangle 7 Cross
8 L1 9 L2 10 R1 11 R2 12 LX 13 LY 14 L3 15 RX 16 RY 17 R3 18 Options 29 TouchPressed. The `EButton` names in the ELF, e.g. eB_X = 8, are left over from another game.)
Robust: at run time pick the lowest b with `_buttonMap[b] == signal` instead of the fixed masks (remaps come from data, I).
Edges: keep our own last copy of `_buttonState` (`pressed = cur & ~last`), so it does not matter how often our hook runs.
Paused game: frame time `[0x2ab4100]+0x28` (ghost notes); skip our buttons while paused.

### Stopping the game from using those buttons
| Button | Game use | Block |
|---|---|---|
| Square, L1, R1, D-pad, sticks, Cross, Circle | kart (brake, hop/drift, item, steer) | kart lock above (I) |
| Triangle | boost bar style toggle 0xe31e40 -> 0xe31f80 | set `CRacerHudComponent._allowSpeedometerToggle` (+0x8a) = 0; 0xe31e51 returns when 0 (V). Restore after |
| camera look back / near-far | 0xc88b80 | skipped while the lock is on (V) |
| Options | pause menu | keep |
Fallback if something still reacts: hook the end of the controller loop in 0xa3b350 and clear the bits (and `_deflections`)
of the primary controller while spectating and not paused (our copy is taken first). Not needed for v1.

## 4. HUD
- Online spectator HUD = `CGuiBehaviorSpectatorHud` (0x98): only `_playerText` (+0x90), the watched player's name (V). No speed/boost binding.
- Race HUD = `CRacerHudComponent` (0x90, meta slot 0x2ad4940) on the local player's kart: +0x18 entity, +0x28 `_racer`, +0x38 HUD project,
  +0x8a `_allowSpeedometerToggle`, +0x8c `_boostBarIndex`. Found with `0x3dbf90(kart, [0x2ad4940], 0)` (V).
- Speedometer needle: `0xe32050(hud, index)` sends `CGuiEventRacerSetSpeedometerVisible` with `_entity` = `hud[+0x18]` (0xe32185, V);
  the Speedometer VSC reads `getForwardSpeedValues_VSC(_entity)` (Reserves notes). To bind it to the ghost (I): swap `hud+0x18` to the ghost
  kart for the one call `0xe32050(hud, hud->_boostBarIndex)`, then restore. Ghost speed may read 0 (kinematic, see 1).
- Boost bar fill, lap, position, timer stay the player's (I). The Reserves mod also reads `hud+0x18` (shows the player's kart).

## Plan
1. **Per-frame hook: 0xea2e63** `e8 08 b7 12 00` (`call 0xfce570`, rdi = `[0x2abc3d0]` CGhostManager) inside 0xea2d90; runs only in
   Time Trial (`_raceMode == 2`, 0xea2daa). Patch: `call cave`; the cave runs the spectator frame, then `jmp 0xfce570` (rdi kept).
   0xea2e63 is not a jump target (0xea2e5c is). Not used by the Reserves mod (it hooks 0xe5aa30).
2. Start of spectating (first frame after race start, `_isPlaying` = mgr+0x14): find the player kart, take the lock request, set
   `_allowSpeedometerToggle = 0`, pick ghost 0, retarget the camera.
3. Each frame: read the primary controller, edges from our own copy; L1/R1 -> previous/next entry of `mgr->_playingGhosts` (+0x40) whose
   `_kart` resolves -> retarget recipe; Square/D-pad/Triangle for the other features. Nothing while paused.
4. Leaving (level exit 0xf9b450 or the end): clear the override (`0x14bea20(cam+0x278, 0)`), release the lock, restore +0x8a.
   Keep no pointers across levels.

### Open risks
- Lock request effect on driving is inferred (same calls as race end); test that the kart does not move and no ghost is saved.
- Camera on a kinematic ghost: offsets may look stiff; the switch may swoop (try `_resetOnNextUpdate`).
- `actor+0x14c = 1` on the watched ghost (camera side effect) may change how the game treats it (maybe player-only sounds: could even help).
- Audio focus may stay on the player's kart (CGameSound instance not found yet).
- Speedometer on a ghost may show 0; boost bar/HUD remain the player's.
- Button bits can be remapped by data; read `_buttonMap` at run time.
- During the start countdown/intro the active camera may not be the CKartCamera: check the meta each time, retry later.
