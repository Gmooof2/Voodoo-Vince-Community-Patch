# X3DAudio exports: jump to the real system DLL's functions, loading it on first use.
# A jump keeps the caller's stack and registers exactly as they were, so the thunks
# work whatever the real calling convention is. ecx/edx are preserved around the
# one-time load in case they carry arguments.
    .text

    .globl _X3DAudioInitialize
_X3DAudioInitialize:
    cmpl $0, _vf_real_init
    jne 1f
    pushl %ecx
    pushl %edx
    calll _vf_ensure_real
    popl %edx
    popl %ecx
1:  jmpl *_vf_real_init

    .globl _X3DAudioCalculate
_X3DAudioCalculate:
    cmpl $0, _vf_real_calc
    jne 1f
    pushl %ecx
    pushl %edx
    calll _vf_ensure_real
    popl %edx
    popl %ecx
1:  jmpl *_vf_real_calc
