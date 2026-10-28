# Kart sounds and voice lines: research (USA CUSA13795 1.21)

ELF addresses of `CTRNF/out/eboot.elf` (patch XML = ELF + 0x400000). Read from code and assets only; nothing was run.
**V** = verified (read in code or asset data). **I** = inferred (likely, not proven). Register args are SysV (rdi, rsi, rdx, rcx, r8; floats xmm0..).
Scope (owner, 2026-10-28): "you don't need to change at all what the ghost kart does per se, just add sfx". So this file is about
playing sounds and voice lines on the spectated ghost, not about giving it boosts/hits/VFX.

## 1. Big picture (V)
Kart audio is made of four layers. None of them checks `_isGhost` (searched: only `isGhost_VSC` 0x112f970 reads +0x260, and only
`common_kart_ghost_timetrial.igz` uses it, for the ghost look VFX).
| Layer | Where | Driven by |
|---|---|---|
| Voice lines (VO) | VSC graph `vsc/common/common_Driver_Vo_component.igz` (in every driverX.pak, newest copy in update.pak) on the **driver** entity (I) | kart messages: `COnKartBoostMessage`, `COnKartDamageStart`, `COnKartShieldedHitStart`, `CRacerChangedPositionMessage`, `CRaceStartMessage`, `CRacerFinishedRaceMessage`, `CCustomEvent "TrickJump_Started"` |
| Kart one-shot SFX | VSC graph `vsc/common/Kart/Vehicle_Audio_sfx.igz` (graph name `common_Kart_sounds`, basekart.pak / update.pak) | `COnKartBoostMessage`, `COnKartVsKartCollisionMessage`, `COnKartDamageAddedEffectStart/End`, `COnKartDriftWobbleStart/EndMessage`, `CCustomEvent` "spinout_started" / "brake_started" / "warp_started", race start/finish |
| Animation-event SFX | `behavior_events/Karts/Kart*_combat.igz` (CCESound / CCECustomEvent on kart anim clips) | kart animation clips: `boost_pad_in`, `break_loop` (sends "brake_started"), `collision_f` (front wall hit), `explosion`, `hop_in/out`, `jump_in/out` (sends "TrickJump_Started"), `land`, `spinout_left` (sends "spinout_started"), `warp_partial` |
| Engine / tires / drift / boost loops | C++ `CVehicleAudioComponent` + `CAudioGraph` / `CAudioGraphDriver` modes, `CKartEffectsComponent` (trails, water, mud, exhaust) | entity velocity and kart component **state** (boost list, drive state, drift timer, on ground) every frame |

All of them end in the same C++ play path, which applies the sound's own rules (chance, cooldown, max playbacks, 3D range).

## 2. Play functions a patch can call (V unless marked)
| ELF | Call | Notes |
|---|---|---|
| **0x1024340** | `playSoundByName(CEntity *e, const char *name, CSoundInstance **out, void *cbHandleSlot)` | Core of `CAudioNodes::playSoundByName_v2_VSC` (0x1094080). Releases old `*out`, returns early if name empty or e null. Finds the CSound by name for this entity (0x287780, see below), takes the position from `e->vtable[0x150]`, then 0x1023c90. `out` = pointer to a zeroed 8-byte slot; result has a reference (release with 0x149d260 when not kept). `cbHandleSlot` = pointer to a zeroed 8-byte slot = no "finished" callback (I). |
| 0x10241f0 | `playSound(CEntity *e, CSound *snd, CSoundInstance **out, void *cbHandleSlot)` | Same with a CSound object (core of `playSound_v2_VSC` I). |
| 0x1023c90 | `(CSoundInstance **out, CEntity *e, const float pos[3], CSound *snd, void *cbHandleSlot)` | Returns at once if `[0x94efa0()+0xd0]` is set (I: audio off / server). Calls 0xae2fc0. |
| 0xae2fc0 | `(CSoundInstance **out, CGameSound *gs = [0x2abb750], igHandle *soundHandle, CEntity *e, const float pos[3])` | Lowest common play call. Kart C++ code calls it with `pos = {0,0,0}` (attached to entity). |
| 0x2857b0 | CSound start check (inside the play path) | **The game's chance/cooldown rules (V):** returns "no play" when `|_cooldown| > eps` and `_lastPlayTime + cooldown > now`; when `_playPercentage < 100` it rolls the sound system RNG (`[sys+0x1b0]`, Park-Miller) against it; `_maxPlaybacks == 0` = never; 3D sounds (flags 0x3000) are culled by distance to the listeners vs `_max3d`. So any call through 0x1024340 obeys the same rules as the game. |
| 0x287780 | `findSound(igHandle *out, CSoundSystem *sys = 0x255bd0(), CEntity *e, igString *name)` | Asks `sys->[0x20](list, e)` for the banks that belong to `e`, searches them, then all loaded banks (`sys+0xd0`). Per-character VO names (e.g. "VO2_PLR_Boosting") resolve to that character's bank (I: through the entity). |
| 0x255bd0 | `CSoundSystem *getSoundSystem()` | singleton (meta CSoundSystem 0x2a23318) |
| 0x1024e80 | `setVehicleVolume(CEntity *kart, xmm0 volume, xmm1 fadeTime)` | `CVehicleAudioComponent` +0x7c wanted volume, +0x80 fade. `setVehicleVolume_VSC` core. |
| 0x1024ee0 | `setVehiclePitch(CEntity *kart, xmm0 pitch, xmm1 fadeTime)` | +0x8c wanted pitch, +0x90 fade. Engine pitch scale on top of the graph. |
| 0x1094280 | `stopSound_VSC` wrapper | (C++ stop core not traced; I: release / stop on CSoundInstance) |
| 0x10cffe0 / 0x102f6c0 | `isLocalHero(e, bool *yes, bool *no)` | true only for a CActor with a player whose local index `[player+0x20] <= 7` (0xc02470, 0xc02710). **Ghost = false**, so all VSC graphs pick the `_NPC` sounds for ghosts. |
| 0xfd3940 / 0xfd3d80 | `getDriver(kartEntity)` / `getKart(driverEntity)` | Cores of `getDriver_VSC` / `getKart_VSC`. The VO graph calls `getKart` on its owner, so its owner is the driver entity (I). |
| 0xd32160 | `sendMessage(CEntityMessage *msg, CEntity *target, CEntity *sender)` | How the game sends kart messages (e.g. 0xe2e904: `msg, kart, kart`). |

CSound (0xe0, meta 0x2a22de0; fields from the field callback 0x16bb848): +0x20 `_lastPlayTime`, +0x30 `_name`, +0x3c `_playPercentage` (u8),
+0x3d `_maxPlaybacks`, +0x3e `_priority`, +0x44 `_delay`, +0x48 `_soundGroupName`, +0x60 `_subSoundList` (variants, `_playFeature` RANDOM/NOREPEAT/SHUFFLE),
+0x68 `_volume`, +0x78 `_channelGroupName`, +0x84 `_cooldown` (float s), +0x90/+0x94 `_min3d/_max3d`.
CSoundInstance (0xd8): +0x18 `_entity`, +0x20 `_sound`, +0x60 `_position`, +0x79 `_paused`, +0xb8 `_stopDelegate`.
**Note (V):** cooldown and `_lastPlayTime` live on the CSound in the bank, so they are shared by every kart that uses the same bank.

## 3. Voice lines: the game's rules (V, decoded from common_Driver_Vo_component.igz, update.pak copy)
Graph state: `busy` (Bool_xkbc3tm0) = a VO line is playing; set to 1 after each play, set to 0 by the line's "finished" callback.
`raceStarted` (Bool_s3jbgilm) = set by CRaceStartMessage. PLR/NPC choice = `isLocalHero(kart)` (ghost -> NPC).
| Trigger | Rule | Line (PLR / NPC) |
|---|---|---|
| COnKartBoostMessage | `!busy` and `_amount > 200` | `VO2_PLR_Boosting` / `VO2_NPC_Boosting` |
| CCustomEvent "TrickJump_Started" (kart anim `jump_in`) | after 0.3 s, no busy test | `VO2_PLR_BigAir` / `VO2_NPC_BigAir` |
| COnKartDamageStart, type 1 **WallHit** (the "Ouch") | stop the current VO lines, after 0.2 s | `VO1_PLR_BeenHit_Light` / `VO1_NPC_BeenHit_Light` |
| COnKartDamageStart type 0 SpinOut | stop current, 0.1 s | `VO1_*_SpinningOut` |
| type 2 Explosive, 6 Poisoned | stop current, 0.3 s | `VO1_*_BeenHit` |
| type 3 Eaten / 4 Fire / 5 Electrified / 7 Ntropied / 8 Crushed | stop current, 0.3 s | `VO1_*_BeenHit_Eaten / _Burned / _Electrified / _Ntropied / _Crushed` |
| COnKartShieldedHitStart (shield damaged) | stop current, 0.3 s | `VO1_*_ShieldedHit` |
| CRacerChangedPositionMessage | `raceStarted`, `!busy`, new position better | `VO2_*_PassingRacer` (not useful for one ghost) |
| race finish | win / lose | `VO1_PLR_Win/Lose`, `VO2_NPC_Win/Lose` |
EKartDamageType (enum table V): 0 SpinOut, 1 WallHit, 2 Explosive, 3 Eaten, 4 Fire, 5 Electrified, 6 Poisoned, 7 Ntropied, 8 Crushed, 9 CrushedAndStopped, 10 GreenBeaker.
On top of the graph, the CSound data (Crash bank `sounds/banks/Characters/CHAR_Driv_Crash_bank_Lv.igz`, all characters look alike) adds (V):
| Sound | play% | cooldown | delay | variants | 3D |
|---|---|---|---|---|---|
| VO2_PLR_Boosting / NPC | 100 | **30 s** | 0.3 | 3 | PLR 2000-8000, NPC 3000-8000 |
| VO2_*_BigAir | 100 | 30 s | 0.15 | 3 | |
| VO2_*_PassingRacer | 100 | 40 s | 0.15 | 3 | |
| VO1_PLR_BeenHit* | 100 | 3 s (NPC: 0) | 0-0.3 | 1-5 | |
| VO1_*_SpinningOut, ShieldedHit, Win/Lose | 100 | 0 | | 2-3 | |
So "not every boost has a voice line" = boost speed > 200, no other VO playing, and the 30 s CSound cooldown; the variant is picked by the CSound.
PLR lines: group `VO_default`, channel `VO_VERB_PLR`, priority 255. NPC lines: `VO_npc`, `VO_VERB_NPC`, priority 190 (other mix).

## 4. Kart one-shot SFX (V names from banks; graph logic V where stated)
`Vehicle_Audio_sfx.igz` takes its CSound objects from component data fields (not by name), and picks PLR/NPC with isLocalHero. Logic read:
- COnKartBoostMessage: `_amount > 0` -> local: play boost sound A, then extra sounds + priority DSP override; non-local: sound B (exact names not traced; the graph's own data).
- "Boost sweetener": counter 0..2 picks `COM_VEH_Boost_Sweetener(_01/_02)_PLR/_NPC`, reset after 4 s (graph event; I: driven by chained boosts).
- COnKartVsKartCollisionMessage: `_impactSpeed > 1000`, gate, `> 3000` hard else medium (other kart sounds). Not needed in Time Trial.
- COnKartDriftWobbleStart/End: start/stop two loops, pitch set with `setSoundPitch` / `setVehiclePitch`.
- CCustomEvent "brake_started": brake sound by `getMaxSpeedRatio >= 0.4` (fast) / slower.
Names in `sounds/banks/Vehicle/VEH_common_bank.igz` (permanent.pak, update2.pak) with PLR and NPC variants, e.g.:
`COM_VEH_Hop_Jump_*_set`, `COM_VEH_Hop_Land_*_set`, `COM_VEH_jump_*_set` (cooldown 0.1), `COM_VEH_land_*_set` (0.1), `COM_VEH_Impact_Med/Hard_*_set` (0.1),
`COM_VEH_Collision_impact_*_set`, `COM_VEH_Skid_*_set` (PLR 0.3 s, NPC 1 s), `COM_VEH_Skid_Chirp_*_set` (play% 80), `COM_VEH_Skid_drift_*_set/_loop`,
`COM_VEH_Skid_squeal_*_loop`, `COM_VEH_Drift_flame_burst_*_set` (0.05) and `_loop_Fire01..03` (drift charge levels), `COM_VEH_Drift_Meter_Fail_Race_*_set`,
`COM_VEH_Boost_Sweetener*`, `COM_VEH_Boost_Wheelie_out_*_set`, `COM_VEH_Brake(_Fast/_Slow)_*_set`, `COM_VEH_Spinout_Accent/TireSkid_*_set`,
`VEH_surfacetype_<grass|dirt|mud|sand|snow|ice|rock|metal|wood|water...>_*_loop/_in` (surface tires), `COM_VEH_Submerged_*`, `COM_VEH_Gearshift_set_*`.
Other banks: `common_bank.igz` `GPI_BoostPad_PLR/NPC`; `VEH_common_powerups_bank.igz` `PU_TurboBoost_In_PLR/NPC`; per kart `VEH_<Kart>_bank.igz`
`veh_<kart>_eng_idle_loop`, `veh_<kart>_eng_loop_01/02` (+`_NPC`). (The full list of boost/start sounds was not finished: the start-boost and
drift-boost activation sounds are probably the engine graph's `_boostActivationSound` / stage `_activationOneShotSound`, see 5.)

## 5. Engine, tires, drift and boost loops (V unless marked)
- `CVehicleAudioComponent` (0xa0, meta 0x2adc4e8): +0x25 `_soundOn`, +0x68 `_driverList`, +0x70 `_nonLocalPlayerDriverList`, +0x79 `_isLocalPlayer`,
  +0x7c..0x98 volume/pitch (wanted / fade / original / current). Data (+0x10 of comp): +0x10/+0x18 the two driver lists, +0x30/+0x38 sound on/off one-shots,
  +0x50/+0x58 local/non-local drift-fail one-shot, +0x9c `_alwaysUpdateAudio`.
- Init 0xd90470 (skips StartScreen / OnlineLobby / Scrapbook / CharacterSelect): plays sound-on, `_soundOn = 1`, subscribes CEntityUpdateMessage -> **0xd90970**
  and CKartDriftFailMessage -> 0xd90c40 (drift fail one-shot, local or non-local by +0x79). Ghost karts are spawned like other karts, so this runs for them (I).
- Per frame 0xd90970(comp, entity): smooth volume/pitch; list = `_isLocalPlayer ? +0x68 : +0x70`; if `_soundOn`, for each `CAudioGraphDriver`
  0xb4faa0(driver, ?, entity, kartController, vol, pitch); then 0xd90d50 (surface / horn).
- 0xb4faa0 picks the active `CAudioGraphDriverMode` (list in priority order, vtable +0xc0 = shouldActivate(mode, ?, entity, kartController)) and feeds the
  `CAudioGraph` (`CAudioGraphModule` loops cross-faded by percent, pitch per module). Modes: Stationary, SurfaceAccel (stages with one-shots), SurfaceDecel,
  SurfaceReverse, SurfaceDrift (stages = drift charge levels, `_driftActivateSound`, `_driftLoopingSound`, `_driftDeactivateSound`), SurfaceBoost (stages,
  `_boostActivationSound`, `_boostLoopingSound`), InAir, SurfaceBurnout, RubberBurning (start-line rev, `_meterFailOneShot`), StickBased.
- Inputs read by the modes (V for these three): InAir 0xb24420 = `!kartController._onGround` (+0x214); Stationary 0xb28b30 = forward speed from
  `CEntity._velocity` (+0x60) below `_noVelocityThreshold`; Boost 0xb25b00 = `CKartBoostComponent` list (+0x28) not empty and last boost not a drift boost
  (data +0x3e); Drift 0xb27500 = `kartController._driveState` (+0xe0, 3..4 = drift, I), `_successfulDriftBoostCount` (+0xf0), `_driftTimer` (+0xf8 -> +0x14 level) and the boost list.
- `CKartEffectsComponent` (0x200): +0x25 `_isLocalPlayer` picks `CKartVfxObject._localSound` (+0x20) or `_otherSound` (+0x28) in 0xe89130 for trails,
  exhaust, water, mud, horn; landing handler 0xe84ec0 (CPlayerLandMessage) plays the landing VFX object sound via 0xe0a450 + 0xae2fc0.
- **Ghosts today (I):** the ghost is moved by setting its transform (0xfce6a0); its velocity, boost list, drive state and on-ground flag stay at rest, so
  the engine sits in the idle/stationary mode with the NPC loops, and no drift/boost/air modes are reached.

## 6. Who sends the kart messages (V)
| Message (meta slot / factory) | Sent by | C++ handlers |
|---|---|---|
| COnKartBoostMessage (0x2ac4080 / 0x1128850): +0x38 `_amount` = boost data speed (+0xc), +0x3c `_isABoostPad` | add boost 0xe2e6b0 (rdi boost comp, rsi CKartBoostData, rdx owner) | CKartEffectsComponent 0xe859f0 (exhaust VFX tier from `_currentBoostSpeed`) |
| COnKartDamageStart (0x2ac50d8 / 0x112ab70): +0x38 kart, +0x40 driver, +0x48 type | damage start 0xe34850 (rdi damage comp, esi type 0..10) from 0xe330e0 | 0xea7060 (racer: GUI take damage) |
| CKartDriftFailMessage (0x2ac5820 / 0x112d0b0) | drift-boost try 0xe2f9a0 (also sends CGuiEventOctaneOnDriftBoost) | vehicle audio 0xd90c40 |
| CKartWallContactStart/EndMessage (0x2ac5838 / 0x2ac5848) | 0xe92d40 / 0xe91530 (collision response, from 0xe916b0) | 0xdd1ab0 (empty); VSC Kart_victimFX |
| COnKartVsKartCollisionMessage (0x2ac44a8) | 0xe916b0, 0xe932c0 | VSC only |
| CPlayerLandMessage (0x2ad0860) | kart controller | CKartEffectsComponent 0xe84ec0 (landing sound + VFX) |
| CKartStartTemporaryGhostMessage | add boost 0xe2e6b0, only for boost pads with a cheat/flag (0xfcdf10(gameMgr, 3)) | VSC ghost look |
Message objects: `factory(pool)` (0 = default pool, as for the ghost classes) -> fill fields -> `0xd32160(msg, target, sender)` -> `0x149d260(msg)`.
Subscriptions use `0xbb8cc0(entity)` (dispatcher) then 0xce0640 / 0xcdba50 / 0xcdfb60 / 0xcdf710 `(dispatcher, meta, {this, fn, 0}, 0)`.

## 7. Plan (per event type)
General: call from the game thread inside an existing per-frame hook (e.g. the ghost playback 0xfce570 path or a component update), only for the
spectated ghost. Use **0x1024340(entity, name, &inst, &zeroSlot)** with the **PLR** names (the spectated ghost should sound like the player's kart; the
game itself would choose NPC because `isLocalHero` is false). The CSound rules (chance, cooldown, variants, max playbacks, 3D range) then apply by
themselves in 0x2857b0. Copy the VSC rules from section 3/4 in C (busy flag, thresholds, delays, interrupt).
| Event (detected from inputs + movement) | Call | Open risks |
|---|---|---|
| Boost of any kind (speed jump) | VO: if `!busy` and boost speed > 200 -> `VO2_PLR_Boosting` on the driver entity (0xfd3940), busy until the instance ends. SFX: `COM_VEH_Boost_Sweetener_PLR` (+_01/_02 for chains), pad: `GPI_BoostPad_PLR`, reserves/turbo: `PU_TurboBoost_In_PLR` (I) | Boost speed is not in the ghost file: estimate the "amount" (> 200 rule) from the speed gain or the boost type. Engine boost loop needs the engine graph (see engine row). |
| Wall hit / bump ("Ouch") | stop current VO, after 0.2 s `VO1_PLR_BeenHit_Light` (cooldown 3 s from the bank); SFX `COM_VEH_Impact_Med/Hard_PLR_set` | The game's real trigger is `COnKartDamageStart` type WallHit (from 0xe330e0/0xe34850); its speed threshold was not traced, so the detection threshold must be guessed. |
| Spin-out / other damage | `VO1_PLR_SpinningOut` (0.1 s) + `COM_VEH_Spinout_*_PLR_set` | Not possible in Time Trial (no items); skip unless detected. |
| Jump (hop, firstJump bit) | `COM_VEH_Hop_Jump_PLR_set`; landing `COM_VEH_Hop_Land_PLR_set` | Anim-event sounds (hop_in/out clips) may already play on the ghost if its kart animation plays these clips (unverified): risk of double sounds. Test first. |
| Big jump / ramp (long air time) | `COM_VEH_jump_PLR_set`; after 0.3 s `VO2_PLR_BigAir` (30 s cooldown); land `COM_VEH_land_PLR_set` | Real trigger is the `jump_in` anim event (trick jump); air-time threshold must be guessed. |
| Drift start / sparks / charge levels | `COM_VEH_Skid_drift_PLR_set` at start, `COM_VEH_Skid_drift_PLR_loop` while drifting (keep the instance, stop at end), `COM_VEH_Drift_flame_burst_PLR_set` per successful drift boost, `_loop_Fire01..03` by level; fail `COM_VEH_Drift_Meter_Fail_Race_PLR_set` | Loops need stop: stop core not traced (use `stopSound_VSC` 0x1094280 logic or release; check). Charge level timing comes from DrivingArchetypes data, not traced. |
| Engine / tires | Option 1: write the ghost's `CEntity._velocity` (+0x60) from the movement each frame so the existing (NPC) engine graph follows speed; plus `setVehiclePitch/Volume` 0x1024ee0/0x1024e80. Option 2: set `CVehicleAudioComponent._isLocalPlayer` (+0x79) and `CKartEffectsComponent._isLocalPlayer` (+0x25) to use PLR lists. | Velocity may be read by other code (physics, camera, effects): check that the ghost has no active physics before writing. Switching +0x79 at run time: the other list's loops may not be started (untested). Drift/boost/air engine modes need controller state (`_driveState`, `_onGround`, boost list) that we do not want to change (owner rule): accept idle-based engine + pitch, or play loops by name. |
| Brake | `COM_VEH_Brake(_Fast/_Slow)_PLR_set` by speed ratio >= 0.4 (graph rule) | |
| Surface (grass, dirt...) | `VEH_surfacetype_<mat>_PLR_in/_loop` | Needs the ground material under the ghost: not traced. |
| Start boost | engine RubberBurning/boost mode + boost VO rule | Start-boost sound names not found yet (I: engine graph data). |
Alternative (closest to "the game's own rules", but NPC voices and C++ side effects): send the real messages to the ghost kart with 0xd32160
(COnKartBoostMessage with `_amount`, COnKartDamageStart type 1, CCustomEvent "TrickJump_Started"). Then the game's own VSC graphs decide everything,
but they choose `_NPC` sounds, the boost message also runs the exhaust VFX handler 0xe859f0, and the damage message runs 0xea7060. Not recommended.

## Open risks (summary)
- Exact entity to pass for VO lookup (driver vs kart) and for VEH banks (kart): reasoned from the graph (owner = driver), not run.
- Listener: 3D sounds are culled beyond `_max3d` from the listeners (VO 8000, SFX 1500-6000): the spectator camera must be the listener (I).
- `cbHandleSlot` = zero slot means "no callback" (I). Without a callback, `busy` must be cleared by watching the returned instance (field to test not checked).
- Animation-event sounds may already play on ghosts (kart anim is fed by 0xe54c20): check in game before adding the same sound by name.
- Not traced: wall-hit damage threshold, drift charge timing, start-boost sounds, the stop-sound C++ core, which C++ path plays the boost "whoosh" for the player (engine graph boost mode vs VSC).

How the VSC graphs were read: IGZ objects (`scripts/igz_file.py`), field names from the ELF reflection (class registration 0x15083e0, field callback ->
0x1507850 count + 0x1507980 names/offsets), RNEX values index the EXNM entries whose kind is metafunction/metafield/metaobject, RHND values index all EXNM
entries (handles to the graph's own methods); `igNameList` gives object names (`MessageListener_On_Kart_Boost_Message...`).
