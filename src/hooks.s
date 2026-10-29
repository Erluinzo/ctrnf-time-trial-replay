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
