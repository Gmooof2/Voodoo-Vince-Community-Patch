"""Small Windows Direct3D helpers for building and validating our own shaders.

Uses the Windows compiler and WARP; no SDK, game process, or third-party package
is needed. COM objects are released even when compilation/validation fails.
"""
import ctypes as ct
from pathlib import Path


def method(obj, slot, result, *arguments):
    table = ct.cast(obj, ct.POINTER(ct.POINTER(ct.c_void_p))).contents
    return ct.WINFUNCTYPE(result, ct.c_void_p, *arguments)(table[slot])


def release(obj):
    if obj:
        method(obj, 2, ct.c_ulong)(obj)


def blob_bytes(obj):
    address = method(obj, 3, ct.c_void_p)(obj)
    length = method(obj, 4, ct.c_size_t)(obj)
    return ct.string_at(address, length)


def compile_shader(path: Path, entry: str, profile: str) -> bytes:
    source = path.read_bytes()
    compiler = ct.WinDLL("d3dcompiler_47.dll", winmode=0x800)
    compile_fn = compiler.D3DCompile
    compile_fn.restype = ct.c_long
    compile_fn.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_char_p,
                          ct.c_void_p, ct.c_void_p, ct.c_char_p, ct.c_char_p,
                          ct.c_uint, ct.c_uint,
                          ct.POINTER(ct.c_void_p), ct.POINTER(ct.c_void_p)]
    code, errors = ct.c_void_p(), ct.c_void_p()
    try:
        # Strictness and optimization level 3. Includes are intentionally disabled:
        # distribution shaders must be self-contained, readable source files.
        hr = compile_fn(source, len(source), str(path).encode("utf-8"), None,
                        None, entry.encode("ascii"), profile.encode("ascii"),
                        (1 << 11) | (1 << 15), 0, ct.byref(code), ct.byref(errors))
        if hr < 0:
            detail = blob_bytes(errors).decode("utf-8", "replace") if errors else ""
            raise RuntimeError(f"Shader compilation failed (0x{hr & 0xffffffff:08x}): {detail}")
        return blob_bytes(code)
    finally:
        release(errors)
        release(code)


def validate_pixel_shader(bytecode: bytes):
    """Ask the Windows software GPU to validate the entire supplied DXBC."""
    runtime = ct.WinDLL("d3d11.dll", winmode=0x800)
    create_device = runtime.D3D11CreateDevice
    create_device.restype = ct.c_long
    create_device.argtypes = [ct.c_void_p, ct.c_uint, ct.c_void_p, ct.c_uint,
                             ct.c_void_p, ct.c_uint, ct.c_uint,
                             ct.POINTER(ct.c_void_p), ct.POINTER(ct.c_uint),
                             ct.POINTER(ct.c_void_p)]
    device, context, feature = ct.c_void_p(), ct.c_void_p(), ct.c_uint()
    try:
        hr = create_device(None, 5, None, 0, None, 0, 7,
                           ct.byref(device), ct.byref(feature), ct.byref(context))
        if hr < 0:
            raise RuntimeError(f"WARP creation failed (0x{hr & 0xffffffff:08x})")
        storage = ct.create_string_buffer(bytecode)
        create_ps = method(device, 15, ct.c_long, ct.c_void_p, ct.c_size_t,
                           ct.c_void_p, ct.c_void_p)
        hr = create_ps(device, storage, len(bytecode), None, None)
        # S_FALSE (1) means successful validation without creating an object.
        if hr != 1:
            raise RuntimeError(f"Pixel shader rejected by WARP (0x{hr & 0xffffffff:08x})")
    finally:
        release(context)
        release(device)
