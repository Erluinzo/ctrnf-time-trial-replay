# Ghost Viewer

Watch Time Trial ghosts as a replay in CTR Nitro-Fueled ([OCTR PC port](https://github.com/Ryu7w7/OCTR-NitroFueled-PC)).

In the Time Trial ghost menu, pick ghosts, then press **L1 (Watch Ghosts)**.

| Button | Action |
|---|---|
| L1 / R1 | previous / next lap |
| L2 / R2 | other ghost |
| Square | pause |
| D-pad left / right | rewind / fast forward |
| D-pad up / down | speed (x0.1 to x4) |

Pause menu: switch between Time Trial and Ghost Viewer.

With [Ghost Sharing](https://github.com/Erluinzo/ctrnf-ghost-share), watch your friends' ghosts too.

**Exact ghosts:** a ghost with its full race (`.crr`) is played from it (60 samples a second instead of the ghost's keys), also in normal Time Trials. The game's own replays (OCTR v0.8.2+) keep one for every personal best; for a shared ghost, put its `.crr` in the `RaceReplay` folder (OCTR folder > `user/savedata/1/CUSA13795`, EU `CUSA14876`; game closed). Ghost files are never changed.

Nothing is saved. Works with the [Reserves Meter](https://github.com/Erluinzo/ctrnf-reserves-meter).

## Install
Download the ZIP for your game from [Releases](https://github.com/Erluinzo/ctrnf-time-trial-replay/releases):
`time-trial-replay` (USA, CUSA13795) or `time-trial-replay-eu` (EU, CUSA14876), version 1.21.
In the launcher: Mods > Add files, enable it, restart the game.

## Build
Needs the OCTR checkout at `../CTRNF`, gcc and binutils: `python3 build.py`.

## License
MIT. Copyright (c) 2026 Onur.
