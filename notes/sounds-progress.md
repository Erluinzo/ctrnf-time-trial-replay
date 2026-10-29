# Sounds of the watched ghost: findings (2026-10-29, 01:30-03:20, agent alone, owner allowed until 10:00)

ELF addresses of `CTRNF/out/eboot.elf`. Research before the tests: `kart-events.md` (parts of it are now known wrong, see below).

## How it works now (src/sounds.c, tested in game)
- **The game already plays every sound of every ghost** (hops, landings, drift loops and charge levels, flame bursts, boost
  sweeteners, wheelie, brakes, jumps, boost pads, surfaces, under water, engine, voice lines `VO2_NPC_Boosting/BigAir`), but the
  `_NPC` versions: their mixer channels (`SFX_VERB_NPC`, `SFX_VERB_VEH_NPC`, `VO_VERB_NPC`, group `Veh_npc`/`VO_npc`) are silent in
  Time Trial. That is why ghosts make no sound (owner: "none of them play automatically").
- **Hook 0x28a6b0** (lowest play call: `(CSoundInstance **out, CSoundSystem*, igHandle **sound, CEntity*, const float pos[3],
  xmm0 volume)`; 0xae2fc0 (about 95 callers) and the engine graph modes call it): for the watched ghost's kart or driver
  (0xfd3940), an `_NPC` CSound is swapped for its `_PLR` twin from the same bank (CSound +0xd8 = its bank, bank +0x18 sounds,
  +0x30 name). Then the game's own events, rules, cooldowns and variants apply. Not while paused / D-pad scrubbing.
- **Engine graph**: `CVehicleAudioComponent` (meta slot 0x2adc4e8) +0x79 `_isLocalPlayer` picks the `_PLR` driver list (+0x68)
  instead of `_NPC` (+0x70); +0x7c wanted volume, +0x88 current. Watched ghost: 1 / volume 1 (0 when paused). Others: keep 1
  only until +0x88 reaches 0, then 0 (the flag also makes surface sounds `_PLR`; a `_PLR` loop left un-updated drones at its
  last volume). Ghost engine mode = 0xb24f80 (controller +0x34 set: a velocity-driven mode).
- **Wall hits**: ghosts have no collisions, so the game has none for them. Measured on the player's kart (scripted crashes,
  positions logged at 100 Hz, game sounds logged; scratchpad walllog.py/walldet2.py, 2026-10-29 ~03:55): a head-on hit plays
  `COM_VEH_Collision_impact_PLR_set` at the hit and `VO1_PLR_BeenHit_Light` 0.19 s later (Impact_Med/Hard are kart-vs-kart).
  The game's rule (collision response 0xe916b0, at 0xe91e45; data of `CKartCollisionResponseComponent`: +0x10 1000.0 min
  speed, +0x18 0.819 = cos 35, +0x1c 0.966 = cos 15 while drifting (driveState 3-4)): wall hit -> damage 0xe330e0 type 1.
  `wall_hit()` estimates it from positions: speed before (0.08 s) >= 1000, speed change > 900 pointing back within 35 deg (15
  with R1 held), not a ramp (|vertical change| < 400), not frozen after (out-of-bounds pick-up). Logs: 2/2 head-on hits found
  0.04 s early; 2 zig-zag runs: 1 dead stop without a game sound still counts as a hit (limit); Big Norm and N. Tropy ghosts:
  none. In game: faked stop -> impact 29.97 s, "Ouch" 30.16 s. Owner offered (~03:35) a ghost with wall hits, falls, out of
  bounds: use it to check.

## Crash cause found (all earlier "crashes" in the play calls)
The last argument of 0x1024340 / 0x10241f0 / 0x1023c90 is a "finished" delegate of **two handles (16 bytes)** read by 0x263210,
not an 8-byte slot: passing `&zero8` made the game read stack garbage as the second handle. Pass `void *callback[4] = {0}`.
Delayed sounds (VO with `_delay`, e.g. Boosting 0.3 s) return no instance but play. Sound clock `[sys+0x210]` = 8192 per s;
CSound +0x20 `_lastPlayTime` in that clock, +0x84 cooldown s.

## Facts (V = verified in memory/code)
- `sys+0xd0` global banks (ui, common), `sys+0xd8` all loaded banks; name lookup 0x287780 never searches `sys+0xd8` (V).
- CSound +0x18 list of instance handles; handle +0x28 -> CSoundInstance; instance +0x18 handle -> +0x28 owner entity, +0x60
  position, +0x84 module volume (V). Scratchpad `who.py` listed playing sounds by owner.
- isLocalHero (0x102f6c0, only caller the VSC node 0x10cffe0): actor +0x140 player, player +0x20 <= 7. Used by ~40 scripts
  (music manager, end race cameras, triggers): not hooked on purpose.
- Ghost look: VFX only (`common_kart_ghost_timetrial.igz`), removed/spawned again by `CKartEndTemporaryGhostMessage`
  (factory 0x112da10) / `CKartStartTemporaryGhostMessage` (0x112d8b0) sent with 0xd32160(msg, kart, kart). `_isGhost` is
  `CKartControllerComponent`+0x260 (not the entity), read by much kart logic: never write it.

## Audio test setup (careful: owner's speakers, night)
- Game stream on null sink `ctr_test` (module 536870916), recorded with `parec -d ctr_test.monitor`: `dev/lvl.py`,
  `dev/hear.py`, `dev/phases.py` (one recording, steps 3 s apart). `dev/nomusic.sh` sets music volume 0 (not kept after kill).
- PipeWire remembers mute/sink per app: mute the stream before closing the game (`dev/relaunch.sh` does).
- Clean up: stream back to sink 68 muted, `pactl unload-module 536870916`.
- Test files of the night (shared ghost copies, EU export) were removed and the null sink unloaded at the 04:30 cleanup.
