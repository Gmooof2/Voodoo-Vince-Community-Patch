# Voodoo Vince Remastered, Steam x86 build: restore the area-exit walk-out.
#
# The original LoadScreen::Transition kept updating the world while it faded
# to the load screen, so the VinceWalkOut script walked Vince out of the area.
# The remaster's fade-out (0x48DA30) instead copies the last frame and fades
# over that still image. This runs the walk first: normal world frames with the
# remaster's own load screen drawn over them, its alpha rising from 0 to 1, as
# the prototype did. The remaster's fade then starts from that frame.
#
# Moving objects are lit by the remaster's shadow-casting lights. The main loop
# deletes those as soon as it exits for a load (SHADOW_TEARDOWN at 0x50B78D),
# which left Vince and the gates black during the walk. teardown_gate keeps
# them for an on-foot exit, and walkout_entry deletes them after the walk.
#
# Lives in its own section added by tools/patch_walkout.py. Game functions
# (GET_SCRIPT, GAME_FRAME, ...) and ORIGINAL_SKIP are external symbols that the
# patcher resolves as relative branches and lists; game data is read through
# the image base found at run time. Nothing here needs a base relocation.

    .intel_syntax noprefix

    # Game data (RVAs, read through the image base).
    .set RVA_ENGINE,       0x7653f4   # Swamp engine: +8 clock, +0xc renderer
    .set RVA_GAME,         0x7631ec   # game manager: +0x1c Vince
    .set RVA_LEVEL_LOADER, 0x763494   # LevelLoader: +0x11c load pending, +0x11d with load screen
    .set RVA_RENDER_FRAME, 0x75a730   # object RENDER_FRAME runs on
    .set RVA_WALK_OUT_VT,  0x2f3448   # VinceWalkOut script vtable
    .set RVA_GATE_RETURN,  0x10b792   # after the main loop's call to the gate
    .set MAX_FRAMES,       600        # bound if the clock stops advancing

    # walkout_entry's locals, below its PUSHAD frame.
    .set ELAPSED,  0                  # float seconds walked so far
    .set LEFT,     4                  # frames left before giving up
    .set DURATION, 8                  # float: Transition's fade time (1.0 s)

    .text
# Entered from the fade-out branch of LoadScreen::Transition (EBP = its frame,
# ESI = the load screen); leaves to where that branch went (ORIGINAL_SKIP).
    .globl _walkout_entry
_walkout_entry:
    pushad
    sub esp, 12
    call here
here:
    pop ebx
    sub ebx, offset here@IMGREL       # EBX = image base
    call is_walking_out
    jne done
    # Every caller passes 1.0 s; a 0 would give one transparent frame.
    movss xmm0, [ebp + 0xc]
    movss [esp + DURATION], xmm0
    xorps xmm0, xmm0
    mov ebp, [ebx + RVA_ENGINE]       # EBP is restored by POPAD
    mov eax, [ebp + 8]                # clock
    mov byte ptr [eax + 0x41], 1      # no controller input; the fade-out sets it next anyway
    movss [esp + ELAPSED], xmm0
    mov dword ptr [esp + LEFT], MAX_FRAMES

frame:
    # The game's own frame (main loop 0x50B4D2..0x50B536).
    mov ecx, ebp
    call BEGIN_SCENE
    # Real frame time (never negative), as the original fade used wall-clock time.
    mov eax, [ebp + 8]
    movss xmm0, [eax + 4]
    addss xmm0, [esp + ELAPSED]
    movss [esp + ELAPSED], xmm0
    call GAME_FRAME

    # Render as RENDER_FRAME(1) does, with the load screen drawn before Present.
    push 0
    mov ecx, [ebp + 0xc]
    call DEVICE_BEGIN
    push 0
    lea ecx, [ebx + RVA_RENDER_FRAME]
    call RENDER_FRAME
    movss xmm1, [esp + DURATION]      # alpha = min(1, elapsed / duration)
    movss xmm0, [esp + ELAPSED]
    divss xmm0, xmm1
    divss xmm1, xmm1
    minss xmm0, xmm1
    push eax
    movss [esp], xmm0
    mov ecx, esi
    mov eax, [esi]
    call dword ptr [eax + 8]          # VinceLoadScreen::RenderLoadScene(alpha)
    mov ecx, [ebp + 0xc]
    call PRESENT
    mov ecx, ebp
    call END_SCENE

    dec dword ptr [esp + LEFT]
    jz walked
    movss xmm0, [esp + DURATION]
    comiss xmm0, [esp + ELAPSED]
    ja frame                          # duration > elapsed
walked:
    call SHADOW_TEARDOWN              # deferred by teardown_gate (harmless if it was not)
done:
    add esp, 12
    popad
    jmp ORIGINAL_SKIP

# ZF = 1 when Vince's current script is VinceWalkOut: the exit region gives it
# for an on-foot exit; vehicles, the trolley, deaths and new games do not.
# EBX = image base. Clobbers EAX, ECX, EDX.
is_walking_out:
    mov ecx, [ebx + RVA_GAME]
    jecxz 1f
    mov ecx, [ecx + 0x1c]             # Vince
    jecxz 1f
    call GET_SCRIPT
    xchg eax, ecx
    jecxz 1f
    lea eax, [ebx + RVA_WALK_OUT_VT]
    cmp [ecx], eax
    ret
1:
    inc ecx                           # ZF = 0
    ret

# Replaces the main loop's call to SHADOW_TEARDOWN as it exits. Keeps the
# lights when an on-foot area load follows: one with a load-screen transition
# (LevelLoader +0x11d), which always reaches walkout_entry.
    .globl _teardown_gate
_teardown_gate:
    push ebx
    mov ebx, [esp + 4]
    sub ebx, RVA_GATE_RETURN          # EBX = image base
    mov eax, [ebx + RVA_LEVEL_LOADER]
    cmp word ptr [eax + 0x11c], 0x0101
    jne 2f                            # no area load with a load screen
    call is_walking_out
2:
    pop ebx
    jne SHADOW_TEARDOWN
    ret
