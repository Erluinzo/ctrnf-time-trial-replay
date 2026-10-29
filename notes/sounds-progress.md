# Sounds for the watched ghost: what was tested in game (2026-10-29, 01:30-02:05)

Tested by the agent alone (owner allowed until 10:00), Crash Cove, player ghost (Big Norm) + N. Tropy.
ELF addresses of `CTRNF/out/eboot.elf`. See `kart-events.md` for the research before the tests.

## Results
| Call | Entity | Result |
|---|---|---|
| `0x1024340` play by name (whole path) | ghost **driver** (`0xfd3940(kart)`), voice line | **crash** (in the play path, handle resolve 0x149fcbe) |
| same | ghost **kart**, `GPI_BoostPad_PLR` | **crash** 0x149fcbe |
| same | ghost kart, `COM_VEH_*` names | no sound found (instance 0), no crash |
| same | player's kart, `GPI_BoostPad_PLR` | plays (loudness x1.5 in the recording), instance not returned |
| `0x287780` find sound (lookup only) | ghost kart / player kart / drivers | `GPI_BoostPad_PLR` found for all (global `common_bank`); `COM_VEH_*` and engine names **never found** |
| same | **ghost driver** | **voice lines found**: `VO2_PLR_Boosting`, `VO1_PLR_BeenHit_Light`, `VO2_PLR_BigAir` (different handle than the player's driver: per-character bank) |
| lookup on an entity, then `0x1023c90(&inst, entity 0, ghost position, CSound = [handle+0x28], &zero)` | ghost kart, `GPI_BoostPad_PLR` | **works**: instance returned, loudness x1.2. This is the way to play at the ghost's place. |
| `0x283c10(bank, &igString)` (game's bank search) on the `sys+0xd8` banks | | **crash** in add-ref 0x150a7a5 (lazy bank sort, flag bank+0x2e). Even when only `veh_common*` banks are searched. |
| own search of `veh_common_bank` (bank+0x18 list, CSound +0x30 name, lower case) then `0x1023c90` at the ghost | `COM_VEH_Hop_Jump_PLR_set` | sound object found, **crash** in the start check 0x2857b0 (+0x1224: loop over `[[rbp-0x70]+0x58]` list, add-ref 0x150a7a0 on a bad item) |

## Sound system facts (V = read in memory)
- `CSoundSystem` = `0x255bd0()`, also the pointer at `[0x2a23300]` (V).
- `sys+0xd0`: global banks searched by name (only `ui_bank`, `common_bank`) (V).
- `sys+0xd8`: 30 loaded banks, incl. `veh_common_bank` (188 sounds: hop, land, jump, impact, skid, drift flame burst, brake,
  sweetener, `com_veh_*_plr_set/_npc_set`), `veh_common_powerups_bank` (134: `pu_turboboost_in_plr`...), char banks, kart banks
  (`veh_crashkart_bank`, `veh_championkart_bank`: engine loops), level/music banks (V).
- Name lookup `0x287780`: entity banks via `[sys+0x20]` function (`0xb156e0`), then `sys+0xd0`. It never searches `sys+0xd8` (V).
- Bank: +0x10 name (lower case), +0x18 list of CSound, +0x2e "sorted" flag. CSound vtable ELF 0x244aa40, +0x30 name, +0x60 list of
  one sub-sound object (vtable ELF 0x244c5e0) for both a working (`gpi_boostpad_plr`) and a crashing (`com_veh_hop_jump_plr_set`) sound.
  Why the kart bank sounds crash is not known yet. Ideas: these banks are not "ready" (sorting/initialisation crashes too); the
  sound needs an entity with the kart's audio setup (the game plays them through the kart's VSC graph with CSound handles in
  component data, entity = kart, and through `CVehicleAudioComponent`); compare a working `_npc_set` played by the game for AI.
- Owner: ghost karts play no sounds on their own ("none of them play automatically").

## Audio test setup (careful: owner's speakers, night)
- The owner had **muted** the game's audio stream. For tests the agent loads a null sink (`pactl load-module module-null-sink
  sink_name=ctr_test`, module 536870916 on 2026-10-29), moves the game stream there and unmutes it there; records with
  `parec -d ctr_test.monitor` (`dev/hear.py NAME ENTITY MODE`: triggers 3 times, prints loudness after/before).
- PipeWire remembers the stream's mute/sink per application: mute the stream **before** closing the game (`dev/relaunch.sh` does),
  or a new game starts **unmuted on the owner's speakers** (happened once ~01:40 for about a second).
- Clean up at the end: move the stream back (`pactl move-sink-input <id> 68`, mute it as the owner had it), `pactl unload-module 536870916`.

## Code state (uncommitted test switches, src/sounds.c)
- `SOUND_TEST_ONLY 1` (automatic events off) and `SOUND_TEST_VOICE 0`: set both back for real use.
- Test hook (state `test_go/test_entity/test_mode/test_result/test_used/test_name`, `dev/stest.py NAME ENTITY MODE`):
  entity 0 ghost kart, 1 ghost driver, 2 player kart, 3 player driver; mode 0 play by name, 1 look up only, 2 look up then play at
  the ghost (entity 0), 3 own `veh_common*` bank search then play at the ghost. Remove before release.
- `play()` in sounds.c still uses `0x1024340` (crashes on ghost entities): change it to: voice = look up on the ghost driver +
  `0x1023c90` at the ghost position (works); kart sounds = still open (see above). Common-bank sounds (`GPI_BoostPad_PLR`) work.
TEST FILE added by the agent 2026-10-29 03:10: mod-state/user/data/ghosts/t111_crash_cove_1m21s99_Neox.ctrghost (remove at cleanup)
