"""Build the experimental x86 Steam proxy with LLVM and pinned local headers.

Run fetch_build_headers.py first; shaders are rebuilt fresh into the work dir. No game files are read or
changed. The output contains our DLL only, plus local-test instructions/manifest.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SHADER_NAMES = ["LegacyBaseVS", "LegacyOverlayVS", "LegacyBasePS"] + [
    "LegacyOverlayPS_" + name for name in
    ("default", "add", "desaturate", "illuminate", "multiply", "over", "saturate", "subtract")]


def run(*arguments):
    subprocess.run(list(map(str, arguments)), check=True)


def _load(name):
    # By path, without touching sys.path (C21).
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require_toolchain(tool=lambda name: name):
    # The pinned d3d11.dll is reproducible only with this LLVM (1.3 T6).
    for name, expected in (("clang", "clang version 23.1.3"), ("lld-link", "LLD 23.1.3")):
        lines = subprocess.run([tool(name), "--version"], check=True, capture_output=True,
                               text=True).stdout.splitlines()
        if not lines or (name == "clang" and not lines[0].startswith(expected)) or \
                (name == "lld-link" and expected not in lines[0]):
            raise RuntimeError(f"{name} must be LLVM 23.1.3, found: {lines[:1]}")


def require_headers():
    # build/dependencies must hold exactly fetch_build_headers.py's pinned packages.
    for name, _, expected in _load("fetch_build_headers").PACKAGES:
        package = ROOT / "build/dependencies" / name
        if not package.is_file() or hashlib.sha256(package.read_bytes()).hexdigest() != expected:
            raise ValueError(f"build/dependencies/{name} is missing or not the pinned package; "
                             "run tools/fetch_build_headers.py")


def build(work, bin_dir=None, diagnostic_keys=False):
    """Build d3d11.dll with every intermediate (shaders included) under work; return its bytes.
    Tools come from bin_dir when given (absolute paths), else PATH."""
    def tool(name):
        return str(Path(bin_dir) / (name + ".exe")) if bin_dir else name

    require_toolchain(tool)
    require_headers()
    out = Path(work)
    out.mkdir(parents=True, exist_ok=True)
    if "d3d_tools" not in sys.modules:
        sys.modules["d3d_tools"] = _load("d3d_tools")  # build_shaders.py imports it by name
    with contextlib.redirect_stdout(io.StringIO()):
        _load("build_shaders").build(out / "shaders")
    sdk = ROOT / "build/dependencies/windows-sdk/c/Include/10.0.28000.0"
    vc = ROOT / "build/dependencies/msvc-headers"
    flags = ["--target=i686-pc-windows-msvc", "-O2", "-fno-exceptions", "-fno-rtti",
             "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX"]
    flags += ["-DVINCE_WATER_DIAGNOSTIC_KEYS=" + str(int(diagnostic_keys))]
    for directory in (vc, sdk / "um", sdk / "shared", sdk / "ucrt"):
        flags += ["-isystem", str(directory)]
    flags += ["-I", str(ROOT / "third_party/minhook/include"), "-I", str(ROOT / "src"), "-I", str(out)]
    names = SHADER_NAMES
    manifest = json.loads((out / "shaders/manifest.json").read_text())
    records = {shader["name"]: shader for shader in manifest["shaders"]}
    header = '#pragma once\n#include "LegacyWaterD3D11.h"\nnamespace vince { namespace water {\n'
    for name in names:
        data = (out / "shaders" / (name + ".cso")).read_bytes()
        record = records[name]
        if hashlib.sha256(data).hexdigest() != record["bytecode_sha256"] or \
            hashlib.sha256((ROOT / record["source"]).read_bytes()).hexdigest() != record["source_sha256"]:
            raise ValueError("Rebuild shaders after source changes")
        header += f"static const unsigned char code_{name}[] = {{" + ",".join(map(str, data)) + "};\n"
    values = [f"{{code_{name},sizeof(code_{name})}}" for name in names]
    header += "inline LegacyWaterPrograms EmbeddedNativeWaterPrograms() { return {" + \
        ",".join(values[:3]) + ",{ " + ",".join(values[3:]) + " }}; }\n}}\n"
    (out / "NativeWaterPrograms.generated.h").write_text(header)
    cpp = ["LegacyWaterNormals", "LegacyWaterSimulation", "LegacyWaterVertexGrid", "LegacyWaterCube",
           "LegacyWaterD3D11", "LegacyWaterSurface", "NativeWaterMesh", "NativeWaterField",
           "NativeWaterMaterials", "NativeWaterScene", "NativeWaterOccupants", "NativeWaterRuntime", "NativeWaterMemoryWin32",
           "NativeWaterHooks", "NativeWaterProxy"]
    objects = []
    for name in cpp:
        target = out / (name + ".obj")
        run(tool("clang++"), *flags, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-c",
            ROOT / "src" / (name + ".cpp"), "-o", target)
        objects.append(target)
    for name in ("buffer", "hook", "trampoline", "hde/hde32"):
        target = out / (Path(name).name + ".obj")
        run(tool("clang"), *flags, "-c", ROOT / "third_party/minhook/src" / (name + ".c"), "-o", target)
        objects.append(target)
    runtime = out / "runtime.c"
    runtime.write_text("int _fltused=0;\n"
        "void __movsb(unsigned char* d,const unsigned char* s,__SIZE_TYPE__ n) {\n"
        " volatile unsigned char* a=d; const volatile unsigned char* b=s;\n"
        " while(n--) *a++=*b++;\n}\n")
    run(tool("clang"), "--target=i686-pc-windows-msvc", "-c", runtime, "-o", out / "runtime.obj")
    objects.append(out / "runtime.obj")
    kernel = """VirtualQuery@12 VirtualAlloc@16 VirtualFree@12 VirtualProtect@16
GetProcessHeap@0 HeapAlloc@12 HeapFree@12 HeapReAlloc@16 HeapCreate@12 HeapDestroy@4
CreateToolhelp32Snapshot@8 Thread32First@8 Thread32Next@8 OpenThread@12 SuspendThread@4
ResumeThread@4 GetThreadContext@8 SetThreadContext@8 CloseHandle@4 GetCurrentProcessId@0
GetCurrentThreadId@0 GetLastError@0 GetCurrentProcess@0 FlushInstructionCache@12 Sleep@4
GetModuleHandleW@4 GetModuleHandleA@4 GetProcAddress@8 LoadLibraryW@4 GetSystemDirectoryW@8
GetModuleFileNameW@12 GetModuleHandleExW@12 DisableThreadLibraryCalls@4 CreateFileW@28
WriteFile@20 InitOnceExecuteOnce@16 ExitProcess@4 GetTickCount@0""".split()
    libraries = []
    imports=[("kernel32", kernel), ("msvcrt", ["memcpy", "memset", "memmove", "memcmp", "strlen", "sqrt", "fmod"])]
    if diagnostic_keys:
        imports.append(("user32",["GetAsyncKeyState@4","GetForegroundWindow@0","GetWindowThreadProcessId@8"]))
    for library, exports in imports:
        definition = out / (library + ".def")
        definition.write_text("LIBRARY " + library + ".dll\nEXPORTS\n" + "\n".join(exports) + "\n")
        target = out / (library + ".lib")
        run(tool("llvm-dlltool"), "-m", "i386", "-k", "-d", definition, "-l", target)
        libraries.append(target)
    dll = out / "d3d11.dll"
    run(tool("lld-link"), "/dll", "/nodefaultlib", "/Brepro", "/machine:x86", "/entry:DllMain@12",
        "/export:D3D11CreateDevice=_D3D11CreateDevice@40", "/out:" + str(dll), *objects, *libraries)
    return dll.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "dist/native-water-preview-v1")
    parser.add_argument("--diagnostic-keys", action="store_true",
                        help="Enable F7 comparison modes in this optional test proxy only")
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError("Choose a fresh output directory")
    out = ROOT / "build/native-preview"
    data = build(out, diagnostic_keys=args.diagnostic_keys)
    names = SHADER_NAMES
    dll = out / "d3d11.dll"
    args.output.mkdir(parents=True)
    (args.output / dll.name).write_bytes(data)
    shutil.copy2(ROOT / "third_party/minhook/LICENSE.txt", args.output / "THIRD_PARTY_NOTICES.txt")
    metadata = {"experimental": True, "target": "Steam PE32 1.14.2.0",
                "diagnostic_keys": args.diagnostic_keys,
                "dll_sha256": hashlib.sha256(data).hexdigest(),
                "modifies_game_executable": False, "shaders": names}
    (args.output / "manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (args.output / "README.md").write_text(
        "# Experimental full water preview\n\nClose the game. Preserve any existing d3d11.dll before installing; "
        "do not overwrite another proxy. Copy only d3d11.dll beside the original Vince.exe.\n\n"
        "Launch and check Unwanted Guests, Jimmy's Fanboat and Midway. VinceWater.log records hook activation and replacement/fallback results. "
        "Report the visible result; a successful draw log does not prove original visual parity.\n\n"
        "Rollback: close the game and remove only this d3d11.dll, or restore your previous proxy. "
        "This preview does not change Vince.exe or config files. Restarting the game is required.\n\n"
        "Unsupported builds/layouts/materials preserve native rendering. Original RNG synchronization, gameplay "
        "impulses, mask/culling and reflection capture matching remain experimental. "
        "Keep THIRD_PARTY_NOTICES.txt with any binary redistribution. Source: workspace README.md.\n")
    if args.diagnostic_keys:
        with (args.output / "README.md").open("a") as instructions:
            instructions.write("\nF7 cycles these comparison modes while the game is focused:\n"
                "1. Water reflects the retail 64px capture (startup)\n"
                "2. Water reflects Remastered's 256px capture\n"
                "3. Everything reflects the retail 64px capture (from the next level load)\n"
                "Then returns to 1. The log records each switch.\n")
    print(f"Built local experimental preview: {args.output}")


if __name__ == "__main__":
    main()
