#!/usr/bin/env python3
"""Prints the last wall-hit sounds the spectator played (state log in src/spectate.h), oldest first."""
import struct, subprocess, sys
STATE = 0x800000000 + 0x2beea80
LOG = 192       # offset of log_n in struct state
pid = int(subprocess.check_output(['pgrep', '-x', 'sacredfire']).split()[0])
mem = open(f'/proc/{pid}/mem', 'rb')
def read(at, n):
    mem.seek(at)
    return mem.read(n)
n = struct.unpack('<i', read(STATE + LOG, 4))[0]
entries = [struct.unpack('<Qfi', read(STATE + LOG + 8 + 16 * i, 16)) for i in range(16)]
for k in range(max(0, n - 16), n):
    name, at, ok = entries[k % 16]
    text = read(name, 64).split(b'\0')[0].decode() if name else '?'
    print(f'{at:7.2f} s  {text:40} {"played" if ok else "no instance"}')
print(f'{n} sounds in total')
