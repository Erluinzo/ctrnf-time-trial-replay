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
- **Wall hits**: ghosts have no collisions, so the game has none for them: `wall_hits()` = speed loss > 700 units/s in 0.3 s
  without brake -> `COM_VEH_Impact_Med/Hard_PLR_set` (from `veh_common_bank`, played with 0x1023c90 at the ghost), then after
  0.2 s `VO1_PLR_BeenHit_Light` by name on the driver (0x1024340). Tested with a faked stop only (no Crash Cove ghost hits a
  wall). Owner offered (~03:35) to record a ghost with wall hits, falls, out of bounds: use it to tune / check the wall-hit
  animation idea (see queued-tasks.md).

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
- TEST FILE added 2026-10-29 03:10: `CTRNF/mod-state/user/data/ghosts/t111_crash_cove_1m21s99_Neox.ctrghost` (copy of the
  owner's export, for the shared ghost test): remove at cleanup.
EU TEST FILES: user/savedata/1/CUSA14876/GhostShare/ (created 03:30 by the EU export test)
EU TEST FILE: user/data/ghosts/t111_crash_cove_1m21s99_Neox.ctrghost (folder user/data/ghosts created 03:31 if it was missing)
