# Ghost Spectator (CTR Nitro-Fueled, OCTR PC port)

A game patch by Onur: watch Time Trial ghosts like a replay. Follow any ghost, switch between ghosts,
pause, rewind, fast forward or slow down, and see and hear what the driver does.
For the USA game, CUSA13795 version 1.21, on the [OCTR port](https://github.com/Ryu7w7/OCTR-NitroFueled-PC).

## How to use
In Time Trial, open the ghost menu of a track and choose the ghosts as usual (your own ghost, the
challenge ghosts, shared ghosts). Then press **L1 (Spectate)** instead of Continue, and Ready.
The race starts without you: the camera follows a ghost.

| Button | What it does |
|---|---|
| L1 / R1 | previous / next ghost |
| Square | pause / play |
| D-pad left / right (hold) | rewind / fast forward (x4) |
| D-pad up / down | playback speed: x0.1, x0.5, x1, x2, x3, x4 |
| Triangle | show / hide the controller |
| Options | the game's pause menu (Restart, Quit) |

## What you see and hear
- **The ghost's time and name**, top left, with the ghost number and the playback speed.
- **A controller** with the ghost's inputs: the buttons it presses light up, and the left stick and the
  bar under it show the steering.
- **The watched ghost looks like a normal kart.** The others keep the ghost look.
- **You are not in the race:** your kart and your icon on the minimap are hidden.
- **The watched ghost sounds like your own kart:** engine, drifts, boosts, jumps, landings, surfaces,
  water, boost pads, and the character's voice lines, with the game's own rules (for example, not every
  boost has a voice line). Wall hits play the impact and the "Ouch" line. The other ghosts stay silent.
- The run stops when the ghost reaches the finish, and you can still rewind. Nothing is saved: the run
  records no ghost and unlocks nothing.

## Install
In the OCTR launcher: Mods > Add files, choose the ZIP from the
[releases](https://github.com/Erluinzo/ctrnf-ghost-spectator/releases), enable it, fully restart the game.
Works together with Ghost Sharing (shared ghosts can be watched too).

## Limits
- Ghost files keep the movement and the inputs, but not the collisions. Wall hits are found from the
  movement with the game's own rule (speed and angle of the hit), so a rare hit may differ from the race.
- One local player.

## How it works
The patch adds hooks to the game:
- the ghost menu: L1 starts the race like Continue, in spectator mode;
- the race start: the player's run is not recorded;
- the ghost clock, every frame: the patch sets the ghosts' play time itself (pause, rewind, speeds),
  points the camera at the watched ghost, locks and hides the player's kart, and draws the HUD with
  copies of the game's own HUD pieces;
- the game's lowest "play a sound" call: the game already plays every sound of every ghost, but in the
  silent "NPC" version; for the watched ghost it plays the player version of the same sound instead.

## Building (developers)
Needs the OCTR checkout at `../CTRNF` (with `out/eboot.elf` from a build), gcc and binutils.
`python3 build.py` writes `package/patches/GhostSpectator.xml`.

## License
MIT, see `LICENSE`. Copyright (c) 2026 Onur.
