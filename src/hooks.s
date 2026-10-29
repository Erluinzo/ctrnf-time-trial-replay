# Entry points the patched game code jumps into (USA CUSA13795 1.21, ELF addresses).
# Game addresses are symbols set by build.py; all references are RIP-relative.

    .intel_syntax noprefix
    .text

# Hooks placed in the middle of game code: keep every register a C call may change (also the
# vector registers) and align the stack, whatever its alignment was.
.macro SAVE
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push rbx
    mov rbx, rsp
    and rsp, -16
    sub rsp, 256
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    movdqu [rsp + \n * 16], xmm\n
    .endr
.endm

.macro RESTORE
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    movdqu xmm\n, [rsp + \n * 16]
    .endr
    mov rsp, rbx
    pop rbx
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
.endm

# The ghost menu entry's event handler (0x9b3830): r15 = entry behavior, r13 = event (+0x18 button
# signal), [rbp-0xa8] = entry placeable. Each hook replaces one "is it this event?" check.

# Replaces "cmp rax, rbx; je button_code" at 0x9b38ae (a button pressed on an entry).
# L1 starts spectating; every other button goes to the game's code (or ghost sharing's hook there).
    .globl hook_menu_button
hook_menu_button:
    cmp rax, rbx
    jne game_menu_after_button
    cmp dword ptr [r13 + 0x18], 17          # L1 (igGuiInput signal, tested by ghost sharing)
    jne game_menu_button
    SAVE
    mov rdi, r15
    mov rsi, [rbp - 0xa8]
    call sp_menu_spectate
    RESTORE
    jmp game_menu_exit                      # as the game does for buttons it does not use

# Replaces "cmp rax, rbx; je focus_gain_code" at 0x9b38da.
    .globl hook_menu_gain
hook_menu_gain:
    cmp rax, rbx
    jne game_menu_after_gain
    SAVE
    mov rdi, r15
    mov rsi, [rbp - 0xa8]
    mov edx, 1
    call sp_menu_focus
    RESTORE
    jmp game_menu_gain

# Replaces "cmp rax, r15; mov rbx, r12; jne exit" at 0x9b3915 (focus lost; r12 = entry behavior here).
    .globl hook_menu_lost
hook_menu_lost:
    cmp rax, r15
    mov rbx, r12
    jne game_menu_exit
    SAVE
    mov rdi, r12
    mov rsi, [rbp - 0xa8]
    xor edx, edx
    call sp_menu_focus
    RESTORE
    jmp game_menu_lost

# Called from 0xfce5a2 in the ghost update (r14 = ghost manager, xmm0 = the frame's game time),
# in place of "vaddss xmm0, xmm0, [r14+0x18]": sp_clock returns the new play time in xmm0, which the
# game stores in [r14+0x18] next. The stack is aligned as at a normal function entry.
    .globl hook_clock
hook_clock:
    mov rdi, r14
    jmp sp_clock

# 0x28a6b0, the game's lowest "play a sound" call (from 0xae2fc0, which about 95 places use, and
# from the engine graphs): (CSoundInstance **out, CSoundSystem *, igHandle **sound, CEntity *entity,
# const float position[3], xmm0 volume). Its first instructions
# (push rbp; mov rbp, rsp; push r15) are replaced by a jump to sp_play (sounds.c), which calls
# game_play_original: those instructions, then the rest of the game's function.
    .globl game_play_original
game_play_original:
    push rbp
    mov rbp, rsp
    push r15
    jmp game_play_continue

# 0xfceac0, the ghost menu's "fill the challenge ghosts" (level, &list), called when the menu opens:
# its first instructions (push rbp; mov rbp, rsp; push r15) are replaced by a jump to sp_menu_open,
# which calls this: those instructions, then the rest of the game's function.
    .globl game_fill_ghosts_original
game_fill_ghosts_original:
    push rbp
    mov rbp, rsp
    push r15
    jmp game_fill_ghosts_continue

# 0xa3b350, the input manager's update (every frame, menus too): its first instructions are replaced by
# a jump to sp_input (spectate.c), which calls this: those instructions, then the rest of the game's function.
    .globl game_input_original
game_input_original:
    push rbp
    mov rbp, rsp
    push r15
    jmp game_input_continue

# 0x3d4dd0, the pad input device's update (reads the pad into its values): same as above, for sp_pad_update.
    .globl game_pad_original
game_pad_original:
    push rbp
    mov rbp, rsp
    push r15
    jmp game_pad_continue

# 0x1041c60 (kart, float *ratio): a kart's powerslide meter, for the HUD script (getDriftRatio_VSC).
# Replaced by a jump to sp_drift_ratio (hud.c), which calls this for the game's own value.
    .globl game_drift_ratio_original
game_drift_ratio_original:
    push rbp
    mov rbp, rsp
    push rbx
    push rax
    jmp game_drift_continue

# 0x1041d90 (kart, ...): a kart's air time values, for the HUD script (getAirTimeData_VSC): same, for sp_air_data.
    .globl game_air_original
game_air_original:
    push rbp
    mov rbp, rsp
    push r15
    jmp game_air_continue

# 0xe32050 (racer HUD, bar style): gives the boost bar's script the HUD's kart: same, for sp_hud_bar.
    .globl game_hud_bar
game_hud_bar:
    push rbp
    mov rbp, rsp
    push r15
    jmp game_hud_bar_continue
