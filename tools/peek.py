#!/usr/bin/env python3
"""Prints the spectator mode's state in the running game (Linux; reads /proc/PID/mem).

The layout must match `struct state` in src/spectate.c.
"""
import struct, subprocess, sys

PROBE = 0xfcc430    # a game function whose first bytes are known: finds the image base
PROLOGUE = bytes.fromhex('554889e541574156415541545348')
STATE = 0x2beea80
# armed active set_up paused speed scrub time end watch ghosts | buttons | manager kart lock hud camera target
# | bit[8] | frames menu_presses camera_sets
LAYOUT = struct.Struct('<6i2f2iQ6Q9B3x3iifi')
SPEEDS = [0.1, 0.5, 1, 2, 3, 4]


def main():
    found = subprocess.run(['pgrep', '-x', 'sacredfire'], capture_output=True, text=True).stdout.split()
    if not found:
        sys.exit('The game is not running.')
    pid = int(found[0])
    with open(f'/proc/{pid}/mem', 'rb') as mem:
        base = None
        for line in open(f'/proc/{pid}/maps'):
            start = int(line.split('-')[0], 16)
            try:
                mem.seek(start + PROBE)
                if mem.read(len(PROLOGUE)) == PROLOGUE:
                    base = start
                    break
            except OSError:
                pass
        if base is None:
            sys.exit('game image not found')
        mem.seek(base + STATE)
        v = LAYOUT.unpack(mem.read(LAYOUT.size))
        mem.seek(base + STATE + 196)
        heard, heard_watch = struct.unpack('<fi', mem.read(8))
        mem.seek(base + STATE + 212)
        played = struct.unpack('<i', mem.read(4))[0]
    (armed, active, set_up, paused, speed, scrub, time, end, watch, ghosts, buttons,
     manager, kart, lock, hud, camera, target, *rest) = v
    bits, (frames, presses, camera_sets, show_pad, go, go_watch) = rest[:9], rest[9:]
    print(f'armed {armed} active {active} set_up {set_up} frames {frames} menu L1 presses {presses}')
    print(f'time {time:.2f} / {end:.2f} s, speed x{SPEEDS[speed] if 0 <= speed < 6 else speed}, '
          f'paused {paused}, scrub {scrub}, watching {watch} of {ghosts} ghosts')
    print(f'buttons {buttons:#x}, bits {list(bits)}; camera sets {camera_sets}; pad {show_pad}, race start {go:.2f} s (ghost {go_watch})')
    print(f'manager {manager:#x} kart {kart:#x} lock {lock:#x} hud {hud:#x} camera {camera:#x} target {target:#x}')
    print(f'sounds: {played} changed to the player version or played; checked up to {heard:.2f} s (ghost {heard_watch})')


if __name__ == '__main__':
    main()
