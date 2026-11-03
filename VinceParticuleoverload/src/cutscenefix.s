# Voodoo Vince Remastered, Steam x86 build: fix the crash when a cutscene is
# skipped (reliably after defeating Janice).
#
# A CutsceneDriver creates CutsceneAnimationScripts and attaches them to the
# actors it animates (0x4D1C20). Each script keeps a raw pointer back to its
# driver at +0x40 and calls it from two virtual methods:
#   slot 0x18 (0x4D1100): driver->vfunc[0x68](script+0x44)
#   slot 0x28 (0x4D1120): driver->vfunc[0x6C](script+0x44, a, b, c, d)
# Skipping a cutscene deletes the driver, but the scripts live on with their
# actors. Their next callback calls into freed memory; once that memory holds
# another object (a SwampScript in every crash dump), the call lands in data
# and the game dies with an execute access violation.
#
# The fix calls the driver only while +0x40 still points at a live
# CutsceneDriver: its vtable lies in .rdata and its RTTI base classes include
# CutsceneDriver (about 38 per-level drivers derive from it). The driver's base
# destructor also clears its vtable pointer, so freed memory that was not
# reused yet fails the check too.
#
# Slot 0x18 is guarded in the script dispatcher (0x42E1A8), not in 0x4D1100,
# because vincedebug verifies 0x4D1100's bytes and its vtable slot.
#
# Lives in its own section added by tools/patch_cutscenefix.py. Game code
# (DISPATCH_NEXT, ...) is reached through external symbols that the patcher
# resolves as relative branches; game data is read through the image base found
# at run time. Nothing here needs a base relocation.

    .intel_syntax noprefix

    .set RVA_RDATA_START, 0x2d6000    # .rdata: vtables and RTTI live here
    .set RVA_RDATA_END,   0x74e000
    .set RVA_DRIVER_TD,   0x75aa84    # type descriptor .?AVCutsceneDriver@@
    .set RVA_ANIM_VT,     0x2d78e0    # CutsceneAnimationScript vtable
    .set MAX_BASES,       64          # sanity bound on an RTTI base class count

    .text
# Replaces the script dispatcher's case that calls a script's slot 0x18
# (0x42E1A8; EBP = its frame, [ebp-8] = the script). For a
# CutsceneAnimationScript whose driver is gone, the call is skipped, as the
# script itself does when the pointer is null. Leaves where the case did.
    .globl _dispatch_guard
_dispatch_guard:
    mov ecx, [ebp - 8]
    mov eax, [ecx]
    call dispatch_base
dispatch_base:
    pop edx
    sub edx, offset dispatch_base@IMGREL
    add edx, RVA_ANIM_VT
    cmp eax, edx
    jne dispatch_call
    push ecx
    mov ecx, [ecx + 0x40]
    call is_live_driver
    pop ecx
    test eax, eax
    jz dispatch_skip
    mov eax, [ecx]
dispatch_call:
    mov edx, [eax + 0x18]
    call edx
dispatch_skip:
    jmp DISPATCH_NEXT

# Replaces the prologue of CutsceneAnimationScript slot 0x28 (0x4D1120,
# thiscall with four stack arguments). The stock body continues at
# DRIVER_CALL with EDX = the script.
    .globl _anim_driver_guard
_anim_driver_guard:
    push ebp
    mov ebp, esp
    mov edx, ecx
    mov ecx, [edx + 0x40]
    call is_live_driver
    test eax, eax
    jz anim_skip
    jmp DRIVER_CALL
anim_skip:
    pop ebp
    ret 16

# Replaces 'mov ecx, edi; call SCRIPT_BASE_DTOR' at the end of CutsceneDriver's
# destructor (0x4D1039, EDI = the driver). Every derived driver ends here.
    .globl _driver_dtor_tail
_driver_dtor_tail:
    mov ecx, edi
    call SCRIPT_BASE_DTOR
    mov dword ptr [edi], 0
    jmp DTOR_RESUME

# ECX = a script's driver pointer. Returns EAX = 1 if it points at a live
# CutsceneDriver (or a class derived from it), else 0. Every RTTI pointer is
# range-checked before it is read. Preserves all other registers.
is_live_driver:
    push ebx
    push esi
    push edi
    push edx
    push ecx
    call live_base
live_base:
    pop ebx
    sub ebx, offset live_base@IMGREL  # EBX = image base
    lea esi, [ebx + RVA_RDATA_START]
    lea edi, [ebx + RVA_RDATA_END]
    xor eax, eax
    test ecx, ecx
    jz live_done
    mov edx, [ecx]                    # vtable
    cmp edx, esi
    jbe live_done
    cmp edx, edi
    jae live_done
    mov edx, [edx - 4]                # complete object locator
    cmp edx, esi
    jb live_done
    cmp edx, edi
    jae live_done
    mov edx, [edx + 0x10]             # class hierarchy descriptor
    cmp edx, esi
    jb live_done
    cmp edx, edi
    jae live_done
    mov ecx, [edx + 8]                # number of base classes
    cmp ecx, MAX_BASES
    ja live_done
    mov edx, [edx + 0xc]              # base class array
    cmp edx, esi
    jb live_done
    cmp edx, edi
    jae live_done
    lea ebx, [ebx + RVA_DRIVER_TD]
live_next:
    dec ecx
    js live_no
    mov eax, [edx + ecx*4]            # base class descriptor
    cmp eax, esi
    jb live_no
    cmp eax, edi
    jae live_no
    cmp [eax], ebx                    # its type descriptor
    jne live_next
    mov eax, 1
    jmp live_done
live_no:
    xor eax, eax
live_done:
    pop ecx
    pop edx
    pop edi
    pop esi
    pop ebx
    ret
