#!/usr/bin/env python3
"""Builds the ghost spectator patch: package/patches/GhostSpectator.xml, and a switched-on test copy
in the game's patches folder.

  python3 build.py [path/to/eboot.elf]     (default: ../CTRNF/out/eboot.elf)

The eboot is only read: to check the original bytes at every write and to compute the
ExpectedSHA256 values the launcher checks before it applies the patch.
"""
import hashlib, re, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ELF = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / 'CTRNF/out/eboot.elf'
XML = HERE / 'package/patches/GhostSpectator.xml'
# The owner tests each build at once: a copy goes into the game's patches folder, switched on.
TEST_COPY = HERE.parent / 'CTRNF/patches/GhostSpectator.xml'
VERSION = '0.3.0'
EBOOT_BASE = 0x400000       # patch XML addresses are ELF addresses + 0x400000

CODE = 0x2319300            # unused zero padding in the code segment (0x2317810-0x233c078), after tomtotom's
CODE_END = 0x2320000        # drift gauge (to 0x23192df), before ghost sharing (from 0x2320000)
STATE = 0x2beea80           # after the game's data (ends 0x2bee66d) and ghost sharing's state (0x2bee680, 0x350 used),
STATE_END = 0x2beef80       # before the reserves meter's state (0x2beef80)
MAX_WRITE = 4096
GAME = {
    'game_handle_resolve': 0x149fca0, 'game_make_handle': 0x14bea20, 'game_release': 0x149d260,
    'game_find_component': 0x3dbf90, 'game_ghost_reset': 0xfce410, 'game_start_recording': 0xfcb9a0,
    'game_camera_instance': 0xc4b110, 'game_active_camera': 0xc4b230, 'game_camera_refresh': 0xc8ade0,
    'game_lock_request': 0xc08100, 'game_player_kart': 0xe50f70, 'game_legend_add': 0xa09d30, 'game_string': 0x1574080,
    'game_string_release': 0x1518e30, 'game_menu_prepare': 0xc2e400, 'game_continue_event_new': 0x1255f90,
    'game_ghost_look_end_new': 0x112da10, 'game_ghost_look_start_new': 0x112d8b0, 'game_send_message': 0xd32160,
    'game_set_entity_hidden': 0x3def00,
    'game_spawn_placeable': 0x253c00, 'game_set_text': 0x268ef0, 'game_set_position': 0x268f90,
    'game_set_scale': 0x269120, 'game_set_pivot': 0x269190, 'game_set_opacity': 0x2694a0, 'game_set_hidden': 0x2694d0,
    'game_play_sound': 0x1024340, 'game_get_driver': 0xfd3940, 'game_sound_system': 0x255bd0,
    'game_play_at': 0x1023c90,
    # places in the ghost menu entry's event handler 0x9b3830 that the hooks jump back to
    'game_menu_after_button': 0x9b38b7, 'game_menu_button': 0x9b3a53, 'game_menu_exit': 0x9b3a34,
    'game_play_continue': 0x28a6b6, 'game_fill_ghosts_continue': 0xfceac6,
    'game_menu_after_gain': 0x9b38e3, 'game_menu_gain': 0x9b3b30, 'game_menu_lost': 0x9b3921,
    # data
    'g_ghost_manager': 0x2abc3d0, 'g_frame': 0x2ab4100, 'g_input_manager': 0x2ac2e90, 'g_players': 0x2ad1160, 'g_kart_manager': 0x2ac57c0,
    'g_kart_camera_meta': 0x2ac42e8, 'g_hud_meta': 0x2ad4940, 'g_vehicle_audio_meta': 0x2adc4e8, 'g_race_settings': 0x2ad4e30,
    'g_string_pool': 0x2af2600, 'g_game': 0x2acde48, 'g_gui_manager': 0x2ac0dc0, 'g_composition_vtable': 0x2431b18, 'g_minimap_player_vtable': 0x2544ea0,
    'g_race_system_meta': 0x2ae34c8, 'g_child_manager_meta': 0x2ab3b68, 'g_world_ready': 0x2a92868, 'g_world_id': 0x2a92888,
    'g_world_no_id': 0x2a922d0, 'g_entity_table': 0x2a92880, 'sp': STATE,
}

# (address, original bytes, new instruction, target symbol, padding): each replaces whole instructions
HOOKS = [
    (0x9b38ae, '4839d80f849c010000', 'e9', 'hook_menu_button', 4),         # ghost menu entry: a button
    (0x9b38da, '4839d80f844d020000', 'e9', 'hook_menu_gain', 4),           # an entry gets focus
    (0x9b3915, '4c39f84c89e30f8513010000', 'e9', 'hook_menu_lost', 7),     # an entry loses focus
    (0xea6390, 'e80b561200', 'e8', 'sp_race_start', 0),                    # race start: start recording the player
    (0xfce5a2, 'c4c17a584618', 'e8', 'hook_clock', 1),                     # every frame: the ghost clock
    (0x28a6b0, '554889e54157', 'e9', 'sp_play', 1),                        # every sound the game starts
    (0xfceac0, '554889e54157', 'e9', 'sp_menu_open', 1),                   # the ghost menu fills its list
]

CFLAGS = ['-c', '-O2', '-std=c11', '-fpie', '-ffreestanding', '-fno-stack-protector',
          '-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-fcf-protection=none', '-mno-red-zone',
          '-fvisibility=hidden', '-fno-tree-loop-distribute-patterns', '-Wall', '-Wextra', '-Werror']
# Only RIP-relative references: the game image is not loaded at a fixed address.
RELATIVE = {'R_X86_64_PC32', 'R_X86_64_PLT32'}

LINKER_SCRIPT = '''SECTIONS {
  . = %#x;
  .text : { *(.text .text.*) *(.rodata .rodata.*) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
'''


def read(elf, va, n):
    # The code segment starts at file offset 0x4000 with address 0.
    return elf[0x4000 + va:0x4000 + va + n]


def run(*command):
    subprocess.run([str(c) for c in command], check=True)


def build_code(tmp):
    objects = []
    for source in ('spectate.c', 'hud.c', 'sounds.c', 'mem.c'):
        obj = tmp / (source + '.o')
        run('gcc', *CFLAGS, '-o', obj, HERE / 'src' / source)
        objects.append(obj)
    obj = tmp / 'hooks.o'
    run('as', '--64', '-o', obj, HERE / 'src/hooks.s')
    objects.append(obj)
    for obj in objects:
        relocs = subprocess.run(['readelf', '-rW', obj], capture_output=True, text=True, check=True).stdout
        kinds = set(re.findall(r'\bR_X86_64_\w+', relocs))
        if kinds - RELATIVE:
            sys.exit(f'{obj.name}: not RIP-relative: {sorted(kinds - RELATIVE)}')
    script = tmp / 'link.ld'
    script.write_text(LINKER_SCRIPT % CODE)
    linked = tmp / 'patch.elf'
    run('ld', '-T', script, '-o', linked, '-e', 'sp_clock',
        *[f'--defsym={name}={address:#x}' for name, address in GAME.items()], *objects)
    sections = subprocess.run(['readelf', '-SW', linked], capture_output=True, text=True, check=True).stdout
    # [Nr] Name Type Address Off Size ES Flg ...: only .text may take space in memory
    for name, size, flags in re.findall(r'\[\s*\d+\]\s+(\S+)\s+\S+\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)\s+[0-9a-f]+\s+(\S*)', sections):
        if 'A' in flags and name != '.text' and int(size, 16):
            sys.exit(f'unexpected section {name}: the code segment cannot hold writable data')
    symbols = {}
    for line in subprocess.run(['nm', linked], capture_output=True, text=True, check=True).stdout.splitlines():
        address, _, name = line.split()
        symbols[name] = int(address, 16)
    binary = tmp / 'patch.bin'
    run('objcopy', '-O', 'binary', '-j', '.text', linked, binary)
    return binary.read_bytes(), symbols


def main():
    elf = ELF.read_bytes()
    with tempfile.TemporaryDirectory() as tmp:
        code, symbols = build_code(Path(tmp))
    if CODE + len(code) > CODE_END:
        sys.exit(f'code is {len(code)} bytes: does not fit before {CODE_END:#x}')
    if read(elf, CODE, len(code)) != bytes(len(code)):
        sys.exit(f'{ELF}: the area at {CODE:#x} is not empty')
    data_end = 0x2a00000 + 0x1ee66d
    if STATE < data_end or STATE + 0x500 > STATE_END:
        sys.exit('state area is not in the free space after the game data')

    lines = []
    for address, original, opcode, target, padding in HOOKS:
        original = bytes.fromhex(original)
        if read(elf, address, len(original)) != original:
            sys.exit(f'{ELF}: the code at {address:#x} is not the expected USA 1.21 code')
        rel = symbols[target] - (address + 5)
        new = bytes.fromhex(opcode) + rel.to_bytes(4, 'little', signed=True) + b'\x90' * padding
        assert len(new) == len(original), hex(address)
        # Signature: the replaced bytes, 8 bytes before them and at least 8 after, unique in the
        # executable (function starts look alike, so the part after may need to be longer)
        for after in range(8, 41, 4):
            signature = read(elf, address - 8, len(original) + 8 + after)
            if elf.count(signature) == 1:
                break
        else:
            sys.exit(f'signature at {address:#x} is not unique')
        lines.append((address, new, signature, 8))
    # The port's loader takes at most 4096 bytes per write (src/probe.c, "bad patch entry")
    for at in range(0, len(code), MAX_WRITE):
        lines.append((CODE + at, code[at:at + MAX_WRITE], None, 0))

    def line(va, value, signature, offset):
        sha = hashlib.sha256(read(elf, va, len(value))).hexdigest()
        extra = f' Signature="{signature.hex(" ")}" SignatureOffset="{offset:#x}"' if signature else ''
        return (f'      <Line Type="bytes" Address="{va + EBOOT_BASE:#010x}" Value="{value.hex()}"'
                f'{extra} ExpectedSHA256="{sha}" />')

    xml = f'''<?xml version="1.0" encoding="utf-8"?>
<!-- Built by build.py from src/: edit those, not this file.
     Ghost spectator: L1 in the Time Trial ghost menu starts a run that only plays the chosen ghosts.
     The camera follows one ghost; time can be paused, rewound, sped up or slowed down. The last
     lines are the new code ({len(code)} bytes). -->
<Patch>
  <TitleID><ID>CUSA13795</ID></TitleID>
  <Metadata Title="Crash Team Racing Nitro-Fueled" Name="Ghost Spectator"
            Note="Watch Time Trial ghosts: L1 in the ghost menu. L1/R1 switch ghost, Square pause, D-pad time and speed."
            Author="Onur" PatchVer="{VERSION}" AppVer="01.21" AppElf="eboot.bin">
    <PatchList>
{chr(10).join(line(*l) for l in lines)}
    </PatchList>
  </Metadata>
</Patch>
'''
    XML.parent.mkdir(parents=True, exist_ok=True)
    XML.write_text(xml, encoding='utf-8')
    print(f'{XML.relative_to(HERE)}: {len(HOOKS)} hooks, code {len(code)} bytes at {CODE:#x}')
    if TEST_COPY.parent.is_dir():
        TEST_COPY.write_text(xml.replace('AppElf="eboot.bin">', 'AppElf="eboot.bin" isEnabled="true">'), encoding='utf-8')
        print(f'test copy, switched on: {TEST_COPY}')


if __name__ == '__main__':
    main()
