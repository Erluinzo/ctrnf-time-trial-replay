#!/usr/bin/env python3
"""Builds the Time Trial Replay Mode patch, one mod package per game edition (as the other mods), and a
switched-on test copy of each XML in the game's patches folder:
  package/patches/TimeTrialReplay.xml           USA CUSA13795 1.21
  package-eu/patches/TimeTrialReplay-EU.xml     EU CUSA14876 1.21, with mod.json and README.txt made from package/
  python3 build.py

Each edition's executable is only read: to check the original bytes at every write and to compute the
ExpectedSHA256 values the launcher checks before it applies the patch. The EU addresses were found with
../CTRNF-Mods/tools/eu_map.py (same functions, moved by 0 to -0x20; data at the same addresses).
"""
import hashlib, json, re, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
GAME_DIR = HERE.parent / 'CTRNF'
VERSION = '0.9.3'
EBOOT_BASE = 0x400000       # patch XML addresses are ELF addresses + 0x400000

CODE = 0x2324000            # unused zero padding in the code segment (0x2317810-0x233c078), after ghost sharing
CODE_END = 0x2338000        # (to 0x2323792), before Time Trial Beat-All (0x2338000) and the reserves meter (0x2339000)
STATE = 0x2beea80           # after the game's data (ends 0x2bee66d) and ghost sharing's state (0x2bee680, 0x350 used),
STATE_END = 0x2beef80       # before the reserves meter's state (0x2beef80)
MAX_WRITE = 4096
GAME = {
    'game_handle_resolve': 0x149fca0, 'game_make_handle': 0x14bea20, 'game_release': 0x149d260,
    'game_find_component': 0x3dbf90, 'game_ghost_reset': 0xfce410, 'game_start_recording': 0xfcb9a0,
    'game_camera_instance': 0xc4b110, 'game_active_camera': 0xc4b230, 'game_camera_refresh': 0xc8ade0,
    'game_lock_request': 0xc08100, 'game_player_kart': 0xe50f70, 'game_legend_continue': 0xa09d36, 'game_string': 0x1574080,
    'game_string_release': 0x1518e30, 'game_menu_prepare': 0xc2e400, 'game_continue_event_new': 0x1255f90,
    'game_ghost_look_end_new': 0x112da10, 'game_ghost_look_start_new': 0x112d8b0, 'game_send_message': 0xd32160,
    'game_set_entity_hidden': 0x3def00, 'game_find_driver': 0xf9c610,
    'game_spawn_placeable': 0x253c00, 'game_set_text': 0x268ef0, 'game_set_sprite_material': 0x265f80, 'game_stream_manager': 0x22d0e0, 'game_find_directory': 0x14fad90, 'game_nav_lose_focus': 0x125c660, 'game_set_position': 0x268f90,
    'game_set_scale': 0x269120, 'game_set_rotation': 0x2690b0, 'game_set_color': 0x269460, 'game_set_pivot': 0x269190, 'game_set_opacity': 0x2694a0, 'game_set_hidden': 0x2694d0,
    'game_play_sound': 0x1024340, 'game_get_driver': 0xfd3940, 'game_sound_system': 0x255bd0,
    'game_play_at': 0x1023c90, 'game_ghost_move': 0xfce6a0, 'game_ghost_inputs': 0xfcea00,
    'game_racer_finished_continue': 0xf351b6, 'game_set_entity_position': 0x3dc8e0, 'game_kart_immobile': 0x10425b0, 'game_rumble_continue': 0xc04ed6, 'game_respawn_continue': 0xe7dd06, 'game_turbo_start': 0xe7cb70, 'game_boost_update_continue': 0xe437b6, 'game_hud_boost_stop': 0xe2d7a0, 'game_speed_continue': 0x1041ce6, 'game_speed_ratio_continue': 0x1041ca6, 'game_kart_settings': 0x9b9ec0, 'game_collision_original': 0xe916b0, 'game_camera_contacts_continue': 0xc8b166, 'game_slide_level_continue': 0xe8c816, 'game_add_boost_continue': 0xe2dd16, 'game_entity_position': 0x3dc720, 'game_entity_angles': 0x3dcfb0, 'game_spline_nearest': 0xe49470, 'game_spline_direction': 0xe490c0, 'game_cos': 0x1be9c58, 'game_sin': 0x1be9c68, 'game_respawn_kart': 0x1041a10, 'game_kart_damage': 0x1041450, 'game_drift_ratio': 0xe8c710, 'game_effect_version': 0xe284d0, 'game_entity_player': 0xcb8b30, 'game_pad_boost': 0xe57120, 'game_hud_set_lap': 0xe63f20, 'game_hud_bar_continue': 0xe32056,
    'game_air_continue': 0x1041d96, 'game_set_entity_angles': 0xbabbf0,
    # places in the ghost menu entry's event handler 0x9b3830 that the hooks jump back to
    'game_menu_after_button': 0x9b38b7, 'game_menu_button': 0x9b3a53, 'game_menu_exit': 0x9b3a34,
    'game_play_continue': 0x28a6b6, 'game_input_continue': 0xa3b356, 'game_pad_continue': 0x3d4dd6, 'game_fill_ghosts_continue': 0xfceac6,
    'game_menu_after_gain': 0x9b38e3, 'game_menu_gain': 0x9b3b30, 'game_menu_lost': 0x9b3921,
    # data
    'g_ghost_manager': 0x2abc3d0, 'g_frame': 0x2ab4100, 'g_input_manager': 0x2ac2e90, 'g_players': 0x2ad1160, 'g_kart_manager': 0x2ac57c0,
    'g_kart_camera_meta': 0x2ac42e8, 'g_hud_meta': 0x2ad4940, 'g_vehicle_audio_meta': 0x2adc4e8, 'g_race_settings': 0x2ad4e30,
    'g_string_pool': 0x2af2600, 'g_game': 0x2acde48, 'g_racer_manager': 0x2ad4b70, 'g_gui_manager': 0x2ac0dc0, 'g_composition_vtable': 0x2431b18, 'g_minimap_player_vtable': 0x2544ea0,
    'g_kart_move_meta': 0x2ac4580, 'g_render_matrix_meta': 0x2acf5d8, 'g_kart_damage_meta': 0x2ac5018, 'game_maintained': 0xe2f050, 'g_crates': 0x2acdc40, 'g_crate_collector_meta': 0x2ab5c78, 'game_crate_enter': 0xeec470, 'g_race_clock_meta': 0x2ad3ec8, 'g_race_system_meta': 0x2ae34c8, 'g_child_manager_meta': 0x2ab3b68, 'g_world_ready': 0x2a92868, 'g_world_id': 0x2a92888,
    'g_world_no_id': 0x2a922d0, 'g_entity_table': 0x2a92880, 'sp': STATE, 'g_spectate_kart': 0x2beef78, 'game_malloc': 0x2807aa8, 'game_free': 0x2807a50, 'game_boost_create': 0xe2e6b0, 'g_boost_meta': 0x2ac4018,
}

# EU addresses of the USA ones that moved (eu_map.py); all others are the same
EU_MOVED = {
    0x125c660: 0x125c650, 0x14fad90: 0x14fad80,
    0xe7cb70: 0xe7cb60, 0xe7dd00: 0xe7dcf0, 0xe7dd06: 0xe7dcf6, 0xe8c710: 0xe8c700, 0xe8c810: 0xe8c800,
    0xe8c816: 0xe8c806, 0xe916b0: 0xe916a0, 0xe92c42: 0xe92c32, 0xe96691: 0xe96681, 0xea6390: 0xea6380,
    0xeb6a37: 0xeb6a27, 0xeec470: 0xeec460, 0xf350b8: 0xf350a8, 0xf351b0: 0xf351a0, 0xf351b6: 0xf351a6, 0xf9c610: 0xf9c600,
    0xfcb9a0: 0xfcb990, 0xfce410: 0xfce400, 0xfce5a2: 0xfce592, 0xfce60e: 0xfce5fe, 0xfce619: 0xfce609,
    0xfce6a0: 0xfce690, 0xfce836: 0xfce826, 0xfce997: 0xfce987, 0xfcea00: 0xfce9f0, 0xfceac0: 0xfceab0,
    0xfceac6: 0xfceab6, 0xfd3940: 0xfd3930, 0x1023c90: 0x1023c80, 0x1024340: 0x1024330, 0x1041450: 0x1041440,
    0x1041a10: 0x1041a00, 0x1041ca0: 0x1041c90, 0x1041ca6: 0x1041c96, 0x1041ce0: 0x1041cd0,
    0x1041ce6: 0x1041cd6, 0x1041d90: 0x1041d80, 0x1041d96: 0x1041d86, 0x10425b0: 0x10425a0,
    0x112d8b0: 0x112d8a0, 0x112da10: 0x112da00, 0x1255f90: 0x1255f80, 0x149d260: 0x149d250,
    0x149fca0: 0x149fc90, 0x14bea20: 0x14bea10, 0x1518e30: 0x1518e20, 0x1574080: 0x1574070,
    0x1be9c58: 0x1be9c38, 0x1be9c68: 0x1be9c48,
}
# EU original bytes where they differ: calls to the same functions from moved code
EU_ORIGINAL = {0xe96691: 'e85a6254ff', 0xfce836: 'e8b5e040ff', 0xfce997: 'e864d2bdff'}
# The owner tests each build at once: a copy goes into the game's patches folder, switched on.
EDITIONS = {
    'CUSA13795': dict(name='USA', elf=GAME_DIR / 'out/eboot.elf', xml=HERE / 'package/patches/TimeTrialReplay.xml',
                      test_copy=GAME_DIR / 'patches/TimeTrialReplay.xml', moved={}, original={}),
    'CUSA14876': dict(name='EU', elf=GAME_DIR / 'orig/CUSA14876-eboot.elf', xml=HERE / 'package-eu/patches/TimeTrialReplay-EU.xml',
                      test_copy=GAME_DIR / 'patches/TimeTrialReplay-EU.xml', moved=EU_MOVED, original=EU_ORIGINAL),
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
    (0xa3b350, '554889e54157', 'e9', 'sp_input', 1),                       # every frame: the controllers are read
    (0x3d4dd0, '554889e54157', 'e9', 'sp_pad_update', 1),                  # every frame: the pad device is read
    (0xa09d30, '554889e54157', 'e9', 'sp_legend_add', 1),                  # a button is added to a screen's legend
    (0xe96691, 'e84a6254ff', 'e8', 'sp_body_sync', 0),                     # a kart gets its physics body's position
    (0xc04ed0, '554889e54157', 'e9', 'sp_rumble', 1),                      # a controller rumble starts
    (0xe7dd00, '554889e54157', 'e9', 'sp_respawn', 1),                     # a kart is respawned
    # a kart's collision response (0xe916b0) is only reached through the callback address set here; the port's
    # Itemless code (src/runtime_itemless.c) hooks that function's first bytes and stops the game if they changed
    (0xe92c42, '488d1567eaffff', '488d15', 'sp_collision', 0),
    (0xe56595, 'e8860b0000', 'e8', 'sp_pad_boost', 0),                     # a kart gets a boost pad's boost
    (0xc8b160, '554889e54157', 'e9', 'sp_camera_contacts', 1),             # the race camera's collisions
    (0xeb6a37, 'e83461fcff', 'e8', 'sp_turbo_start', 0),                   # GO: a kart's start boost
    (0xe437b0, '554889e54157', 'e9', 'sp_boost_update', 1),                # the HUD starts its meter update
    (0xe32826, 'e865975aff', 'e8', 'sp_kart_hud', 0),                      # a slide starts: the kart's HUD
    (0xe2d5cb, 'e8c0e95aff', 'e8', 'sp_kart_hud', 0),                      # a slide ends
    (0xe2fe7b, 'e810c15aff', 'e8', 'sp_kart_hud', 0),                      # a slide boost fails
    (0xe32bad, 'e8de935aff', 'e8', 'sp_kart_hud', 0),                      # a slide boost (its rating)
    (0xe61045, 'e9d62e0000', 'e9', 'sp_player_lap', 0),                    # the player's lap to the HUD
    (0xf351b0, '554889e54157', 'e9', 'sp_player_finished', 1),             # a racer finishes the race (every way there)
    (0xe32050, '554889e54157', 'e9', 'sp_hud_bar', 1),                     # the HUD's boost bar gets its kart
    (0x1041ca0, '554889e55350', 'e9', 'sp_speed_ratio', 1),                # the HUD reads a kart's speed ratio
    (0x1041ce0, '554889e54157', 'e9', 'sp_forward_speed', 1),              # and its speed (speedometer)
    (0xe8c810, '554889e54157', 'e9', 'sp_slide_level', 1),                 # a kart's slide effects level
    (0xe2dd10, '554889e54157', 'e9', 'sp_add_boost', 1),                   # a kart gets a boost
    (0xe48cc1, 'e85a3a59ff', 'e8', 'sp_wrong_way_position', 0),            # WRONG WAY check: the kart's position
    (0xe48cdf, 'e8cc4259ff', 'e8', 'sp_wrong_way_angles', 0),              # and its angles
    (0xe48cd2, 'e8e9030000', 'e8', 'sp_wrong_way_direction', 0),           # and the track's direction there
    (0xe27278, 'e853120000', 'e8', 'sp_effect_version', 0),               # a kart effect picks its player or other-kart version
    (0xb95d46, 'e8e52d1200', 'e8', 'sp_effect_player', 0),                # a player-only effect asks for its kart's local player
    (0x1041d90, '554889e54157', 'e9', 'sp_air_data', 1),                   # the HUD reads a kart's air time (jump meter)
    (0xfce60e, 'e88d000000', 'e8', 'sp_ghost_move', 0),                    # every frame, each ghost: its movement
    (0xfce619, 'e8e2030000', 'e8', 'sp_ghost_inputs', 0),                  # and its inputs
    (0xfce836, 'e8a5e040ff', 'e8', 'sp_place_position', 0),                # a ghost kart is placed: position
    (0xfce997, 'e854d2bdff', 'e8', 'sp_place_angles', 0),                  # and angles
]

CFLAGS = ['-c', '-Os', '-std=c11', '-fpie', '-ffreestanding', '-fno-stack-protector',
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


def build_code(tmp, game):
    objects = []
    for source in ('spectate.c', 'hud.c', 'sounds.c', 'boosts.c', 'mem.c'):
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
        *[f'--defsym={name}={address:#x}' for name, address in game.items()], *objects)
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


def build(title, e):
    elf = e['elf'].read_bytes()
    game = {name: e['moved'].get(address, address) for name, address in GAME.items()}
    with tempfile.TemporaryDirectory() as tmp:
        code, symbols = build_code(Path(tmp), game)
    if CODE + len(code) > CODE_END:
        sys.exit(f'code is {len(code)} bytes: does not fit before {CODE_END:#x}')
    if read(elf, CODE, len(code)) != bytes(len(code)):
        sys.exit(f'{e["elf"]}: the area at {CODE:#x} is not empty')
    data_end = 0x2a00000 + 0x1ee66d
    if STATE < data_end or STATE + 0x500 > STATE_END:
        sys.exit('state area is not in the free space after the game data')

    lines = []
    for usa_address, original, opcode, target, padding in HOOKS:
        address = e['moved'].get(usa_address, usa_address)
        original = bytes.fromhex(e['original'].get(usa_address, original))
        if read(elf, address, len(original)) != original:
            sys.exit(f'{e["elf"]}: the code at {address:#x} is not the expected {e["name"]} 1.21 code')
        rel = symbols[target] - (address + len(bytes.fromhex(opcode)) + 4)
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
<!-- Built by build.py from src/: edit those, not this file.{" EU edition." if e["name"] == "EU" else ""}
     Time Trial Replay Mode: L1 in the Time Trial ghost menu plays the chosen ghosts as a replay.
     The camera follows one ghost; time can be paused, rewound, sped up or slowed down. The last
     lines are the new code ({len(code)} bytes). -->
<Patch>
  <TitleID><ID>{title}</ID></TitleID>
  <Metadata Title="Crash Team Racing Nitro-Fueled" Name="Time Trial Replay Mode"
            Note="Watch Time Trial ghosts as a replay: L1 in the ghost menu."
            Author="Onur" PatchVer="{VERSION}" AppVer="01.21" AppElf="eboot.bin">
    <PatchList>
{chr(10).join(line(*l) for l in lines)}
    </PatchList>
  </Metadata>
</Patch>
'''
    e['xml'].parent.mkdir(parents=True, exist_ok=True)
    e['xml'].write_text(xml, encoding='utf-8')
    print(f'{e["xml"].relative_to(HERE)}: {len(HOOKS)} hooks, code {len(code)} bytes at {CODE:#x}')
    if e['test_copy'].parent.is_dir():
        e['test_copy'].write_text(xml.replace('AppElf="eboot.bin">', 'AppElf="eboot.bin" isEnabled="true">'), encoding='utf-8')
        print(f'test copy, switched on: {e["test_copy"]}')


def eu_package():
    """package-eu/ gets the USA package's README, marker file and manifest, with its own id and name"""
    manifest = json.loads((HERE / 'package/mod.json').read_text())
    assert manifest['version'] == VERSION, 'package/mod.json has another version'
    manifest.update(id=manifest['id'] + '-eu', name=manifest['name'] + ' (EU)',
                    patches=['patches/TimeTrialReplay-EU.xml'], files=[])
    (HERE / 'package-eu/mod.json').write_text(json.dumps(manifest, indent=2) + '\n')
    for name in ('README.txt', 'octr-mod.required'):
        shutil.copy(HERE / 'package' / name, HERE / 'package-eu' / name)


def main():
    for title, e in EDITIONS.items():
        build(title, e)
    eu_package()


if __name__ == '__main__':
    main()
