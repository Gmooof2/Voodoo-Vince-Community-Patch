"""Toolchain pin check for the AIO (docs/ops/toolchain.md). Stdlib only.

Usage: PY aio/toolchain.py check
Prints `ok <name> <value>` per pin; on any mismatch prints `FAIL <name>: ...` per failure and exits 1.
"""
import ctypes
import ctypes.wintypes as wt
import importlib.metadata
import os
import shutil
import subprocess
import sys
import zlib
from pathlib import Path

PYTHON = (3, 15, 0)
LLVM_VERSION = '23.1.3'
LLVM_COMMIT = '0d261d1ca552c95a8f007e061c787ac7132fbcbc'
LLVM_BIN = Path('C:/Program Files/LLVM/bin')
FXC = Path('C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe')
FXC_VERSION = '10.0.26100.9457'
D3DCOMPILER = 'd3dcompiler_47.dll'
D3DCOMPILER_PATH = r'C:\Windows\System32\d3dcompiler_47.dll'
D3DCOMPILER_VERSION = '10.0.26100.9711'
ZLIB_RUNTIME_VERSION = '1.3.1.zlib-ng'  # recompressed-archive bytes depend on it (C0021)
REQUIREMENTS = Path(__file__).with_name('requirements.txt')
LOAD_LIBRARY_SEARCH_SYSTEM32 = 0x800  # VinceWaterFix/tools/d3d_tools.py:28 loads with this winmode


class VS_FIXEDFILEINFO(ctypes.Structure):
    _fields_ = [(n, wt.DWORD) for n in (
        'dwSignature', 'dwStrucVersion', 'dwFileVersionMS', 'dwFileVersionLS', 'dwProductVersionMS',
        'dwProductVersionLS', 'dwFileFlagsMask', 'dwFileFlags', 'dwFileOS', 'dwFileType', 'dwFileSubtype',
        'dwFileDateMS', 'dwFileDateLS')]


def file_version(path):
    ver = ctypes.WinDLL('version.dll')
    ver.GetFileVersionInfoSizeW.argtypes = [wt.LPCWSTR, ctypes.POINTER(wt.DWORD)]
    ver.GetFileVersionInfoW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p]
    ver.VerQueryValueW.argtypes = [ctypes.c_void_p, wt.LPCWSTR, ctypes.POINTER(ctypes.c_void_p),
                                   ctypes.POINTER(wt.UINT)]
    size = ver.GetFileVersionInfoSizeW(str(path), None)
    if not size:
        raise OSError(f'no version info: {path}')
    buf = ctypes.create_string_buffer(size)
    if not ver.GetFileVersionInfoW(str(path), 0, size, buf):
        raise OSError(f'GetFileVersionInfoW failed: {path}')
    ptr, length = ctypes.c_void_p(), wt.UINT()
    if not ver.VerQueryValueW(buf, '\\', ctypes.byref(ptr), ctypes.byref(length)):
        raise OSError(f'VerQueryValueW failed: {path}')
    info = ctypes.cast(ptr, ctypes.POINTER(VS_FIXEDFILEINFO)).contents
    ms, ls = info.dwFileVersionMS, info.dwFileVersionLS
    return f'{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}'


def module_path(dll):
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.GetModuleFileNameW.argtypes = [wt.HMODULE, wt.LPWSTR, wt.DWORD]
    buf = ctypes.create_unicode_buffer(32768)
    if not k32.GetModuleFileNameW(dll._handle, buf, len(buf)):
        raise ctypes.WinError(ctypes.get_last_error())
    return buf.value


def first_line(exe):
    # resolve via PATH first: CreateProcess appends .exe only to names without an extension (ld.lld has one)
    out = subprocess.run([shutil.which(exe) or exe, '--version'], capture_output=True, text=True, check=True).stdout
    return out.splitlines()[0] if out else ''


def check():
    oks, fails = [], []

    def expect(name, cond, value, why):
        (oks if cond else fails).append((name, value if cond else why))

    v = sys.version_info
    bits = 64 if sys.maxsize > 2**32 else 32
    expect('python', tuple(v[:3]) == PYTHON and bits == 64, f'{v.major}.{v.minor}.{v.micro} {bits}-bit',
           f'{v.major}.{v.minor}.{v.micro} {bits}-bit, pinned {".".join(map(str, PYTHON))} 64-bit')

    expect('zlib', zlib.ZLIB_RUNTIME_VERSION == ZLIB_RUNTIME_VERSION, zlib.ZLIB_RUNTIME_VERSION,
           f'{zlib.ZLIB_RUNTIME_VERSION}, pinned {ZLIB_RUNTIME_VERSION}')

    expect('llvm_bin', (LLVM_BIN / 'clang.exe').is_file(), str(LLVM_BIN / 'clang.exe'),
           f'{LLVM_BIN / "clang.exe"} missing')
    which = shutil.which('clang')
    same = bool(which) and os.path.normcase(os.path.realpath(which)) == \
        os.path.normcase(os.path.realpath(LLVM_BIN / 'clang.exe'))
    expect('clang_path', same, which, f'PATH clang is {which}, pinned {LLVM_BIN / "clang.exe"}')

    for name, needle in (('clang', f'clang version {LLVM_VERSION}'), ('ld.lld', f'LLD {LLVM_VERSION}'),
                         ('lld-link', f'LLD {LLVM_VERSION}')):
        try:
            line = first_line(name)
        except (OSError, subprocess.CalledProcessError) as e:
            expect(name, False, '', f'{name} --version failed: {e}')
            continue
        expect(name, needle in line and LLVM_COMMIT in line, line,
               f'{line!r} lacks {needle!r} / commit {LLVM_COMMIT}')

    if FXC.is_file():
        fv = file_version(FXC)
        expect('fxc', fv == FXC_VERSION, f'{fv} {FXC}', f'{FXC} is {fv}, pinned {FXC_VERSION}')
    else:
        expect('fxc', False, '', f'{FXC} missing')

    for label, kw in (('d3dcompiler', {}), ('d3dcompiler_sys32', {'winmode': LOAD_LIBRARY_SEARCH_SYSTEM32})):
        try:
            path = module_path(ctypes.WinDLL(D3DCOMPILER, **kw))
        except OSError as e:
            expect(label, False, '', f'load {D3DCOMPILER} {kw} failed: {e}')
            continue
        fv = file_version(path)
        expect(label, path.lower() == D3DCOMPILER_PATH.lower() and fv == D3DCOMPILER_VERSION, f'{fv} {path}',
               f'loaded {path} {fv}, pinned {D3DCOMPILER_PATH} {D3DCOMPILER_VERSION}')

    for raw in REQUIREMENTS.read_text().splitlines():
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        pkg, pin = (s.strip() for s in line.split('=='))
        try:
            got = importlib.metadata.version(pkg)
        except importlib.metadata.PackageNotFoundError:
            expect(pkg, False, '', f'{pkg} not installed, pinned {pin}')
            continue
        expect(pkg, got == pin, got, f'{pkg} {got}, pinned {pin}')

    for name, value in oks:
        print(f'ok {name} {value}')
    for name, why in fails:
        print(f'FAIL {name}: {why}')
    return 1 if fails else 0


if __name__ == '__main__':
    if sys.argv[1:] != ['check']:
        sys.exit('usage: toolchain.py check')
    sys.exit(check())
