"""Build a guarded, reversible copy; never overwrite the supplied game executable.

Compile the freestanding x86 C payload, link it at its final preferred address,
copy its loadable sections, and merge its relocation entries into the game's.
Only the inspected locally modified Steam executable is accepted.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
import pefile

INPUT_SHA256 = "633eebaf76b7ee8e62f876ce364591be2e3fa6119f5a894c51ce9ab7109f11f5"
BASE = 0x400000
LLVM_BIN = Path("C:/Program Files/LLVM/bin")
LLVM_VERSION = "23.1.3"
MARKER = b"VinceNarrationWait-v1\0"
VTABLE = {
    "nrf_activate": (0x2DD424, 0x4F2310),
    "nrf_tick": (0x2DD42C, 0x4F2380),
    "nrf_timer": (0x2DD430, 0x4F2580),
    "nrf_finish": (0x2DD434, 0x4F2390),
}
SIZE_SITES = [0xF27D4, 0xF27EE, 0xF2611]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def align(n, a):
    return (n + a - 1) // a * a


def relocations(pe):
    pe.parse_data_directories(directories=[5])
    return [(e.rva, e.type) for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])
            for e in block.entries if e.type]


def relocation_bytes(entries):
    pages = {}
    for rva, kind in sorted(set(entries)):
        pages.setdefault(rva & ~4095, []).append((kind << 12) | (rva & 4095))
    result = bytearray()
    for page, values in sorted(pages.items()):
        if len(values) % 2:
            values.append(0)
        result += struct.pack("<II", page, 8 + 2 * len(values))
        result += struct.pack("<" + "H" * len(values), *values)
    return bytes(result)


def wrapper(rva, target_rva, timer=False):
    # Preserve general registers/flags, set DF for the C ABI, recover image base.
    code = bytearray.fromhex("9c60fce8000000005b")
    # call return is wrapper+8; pop EBX then subtract that RVA.
    code += b"\x81\xeb" + struct.pack("<I", rva + 8)
    if timer:
        # Original timer arg is above pushfd(4), pushad(32), return address(4).
        code += bytes.fromhex("ff742428")
    code += bytes.fromhex("5351")  # push base; push original this (ECX)
    code += b"\xe8" + struct.pack("<i", target_rva - (rva + len(code) + 5))
    code += b"\x83\xc4" + bytes([12 if timer else 8])
    code += bytes.fromhex("619d")
    code += bytes.fromhex("c20400") if timer else b"\xc3"
    return bytes(code)


def compile_payload(section_rva, work):
    work.mkdir(parents=True, exist_ok=True)
    compiler = LLVM_BIN / "clang-cl.exe"
    linker = LLVM_BIN / "lld-link.exe"
    for tool in (compiler, linker):
        if not tool.is_file():
            raise ValueError(f"pinned LLVM {LLVM_VERSION} tool not found: {tool}")
        first = (subprocess.run([str(tool), "--version"], capture_output=True,
                                text=True).stdout.splitlines() or [""])[0]
        if f" {LLVM_VERSION} " not in first:
            raise ValueError(f"{tool.name} must be LLVM {LLVM_VERSION}; --version reports: {first!r}")
    obj = work / "narration_wait.obj"
    exe = work / "payload.exe"
    subprocess.run([compiler, "/nologo", "/c", "/O2", "/GS-", "/Zl", "/arch:SSE2",
                    "/clang:-ffreestanding", "/clang:-fno-builtin", "/clang:-m32",
                    f"/Fo:{obj}", str(ROOT / "src/narration_wait.c")], check=True)
    mini_base = BASE + section_rva - 0x1000
    subprocess.run([linker, "/nologo", "/machine:x86", "/nodefaultlib", "/entry:nrf_activate",
                    "/subsystem:native", "/safeseh:no", "/dynamicbase", "/nxcompat",
                    "/timestamp:0", f"/base:0x{mini_base:x}", f"/out:{exe}",
                    *[f"/export:{name}" for name in VTABLE], str(obj)], check=True)
    mini = pefile.PE(data=exe.read_bytes())
    if mini.OPTIONAL_HEADER.ImageBase != mini_base:
        raise ValueError("linked payload has an unexpected base")
    if mini.OPTIONAL_HEADER.DATA_DIRECTORY[1].VirtualAddress:
        raise ValueError("payload must not import any external functions")
    sections = [s for s in mini.sections if s.Name.rstrip(b"\0") != b".reloc"]
    if min(s.VirtualAddress for s in sections) != 0x1000:
        raise ValueError("unexpected payload section layout")
    end = max(s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData) for s in sections)
    payload = bytearray(end - 0x1000)
    for s in sections:
        data = s.get_data()
        offset = s.VirtualAddress - 0x1000
        payload[offset:offset + len(data)] = data
    exports = {e.name.decode(): section_rva + e.address - 0x1000
               for e in mini.DIRECTORY_ENTRY_EXPORT.symbols if e.name}
    if set(exports) != set(VTABLE):
        raise ValueError("unexpected payload exports")
    entries = relocations(mini)
    if any(kind != 3 for rva, kind in entries):
        raise ValueError("unsupported payload relocation type")
    entries = [(section_rva + rva - 0x1000, kind) for rva, kind in entries]
    return payload, exports, entries


def build_patch(data, work=None, identity=True):
    # identity=False (AIO chain): skip only the whole-file pin and TimeDateStamp; every site compare stays
    if MARKER in data:
        raise ValueError("already contains the narration patch")
    if identity and sha(data) != INPUT_SHA256:
        raise ValueError("unsupported executable: original SHA256 differs")
    pe = pefile.PE(data=data)
    if (pe.FILE_HEADER.Machine != 0x14C or pe.OPTIONAL_HEADER.Magic != 0x10B
            or pe.OPTIONAL_HEADER.ImageBase != BASE
            or identity and pe.FILE_HEADER.TimeDateStamp != 0x5996A9BB):
        raise ValueError("unsupported executable identity")
    if pe.OPTIONAL_HEADER.DATA_DIRECTORY[4].VirtualAddress:
        raise ValueError("signed executable is unsupported")
    old_relocs = relocations(pe)
    for name, (rva, original_va) in VTABLE.items():
        if pe.get_data(rva, 4) != struct.pack("<I", original_va) or (rva, 3) not in old_relocs:
            raise ValueError(f"unexpected vtable entry or missing relocation: {name}")
    for rva in SIZE_SITES:
        if pe.get_data(rva, 2) != b"\x6a\x74":
            raise ValueError("unexpected flythrough allocation/free size")
    header_offset = pe.sections[-1].get_file_offset() + 40
    first_raw = min(s.PointerToRawData for s in pe.sections if s.SizeOfRawData)
    if (header_offset + 40 > min(first_raw, pe.OPTIONAL_HEADER.SizeOfHeaders)
            or any(data[header_offset:header_offset + 40])):
        raise ValueError("insufficient unused PE section-header space")
    sa, fa = pe.OPTIONAL_HEADER.SectionAlignment, pe.OPTIONAL_HEADER.FileAlignment
    section_rva = align(max(s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)
                            for s in pe.sections), sa)
    payload, exports, new_relocs = compile_payload(section_rva, work or ROOT / "build/payload")
    hooks, wrappers = {}, {}
    for name, (rva, original_va) in VTABLE.items():
        payload += b"\xcc" * (align(len(payload), 16) - len(payload))
        hook_rva = section_rva + len(payload)
        code = wrapper(hook_rva, exports[name], timer=name == "nrf_timer")
        wrappers[name] = {"rva": hook_rva, "size": len(code)}
        hooks[name] = BASE + hook_rva
        payload += code
    payload += MARKER
    payload += b"\0" * (align(len(payload), 4) - len(payload))
    relocation_offset = len(payload)
    merged = relocation_bytes(old_relocs + new_relocs)
    payload += merged
    raw_offset = align(len(data), fa)
    raw_size = align(len(payload), fa)
    result = bytearray(data)
    result += b"\0" * (raw_offset - len(result)) + payload
    result += b"\0" * (raw_offset + raw_size - len(result))
    result[header_offset:header_offset + 40] = struct.pack(
        "<8sIIIIIIHHI", b".vnrfix\0", len(payload), section_rva,
        raw_size, raw_offset, 0, 0, 0, 0, 0x60000020)
    struct.pack_into("<H", result, pe.FILE_HEADER.get_field_absolute_offset("NumberOfSections"),
                     pe.FILE_HEADER.NumberOfSections + 1)
    struct.pack_into("<I", result, pe.OPTIONAL_HEADER.get_field_absolute_offset("SizeOfImage"),
                     align(section_rva + len(payload), sa))
    struct.pack_into("<I", result, pe.OPTIONAL_HEADER.get_field_absolute_offset("SizeOfCode"),
                     pe.OPTIONAL_HEADER.SizeOfCode + raw_size)
    directory = pe.OPTIONAL_HEADER.DATA_DIRECTORY[5]
    struct.pack_into("<II", result, directory.get_file_offset(),
                     section_rva + relocation_offset, len(merged))
    for name, (rva, original_va) in VTABLE.items():
        struct.pack_into("<I", result, pe.get_offset_from_rva(rva), hooks[name])
    for rva in SIZE_SITES:
        # Append two dwords: packed state flags and elapsed game seconds.
        result[pe.get_offset_from_rva(rva) + 1] = 0x7c
    patched = pefile.PE(data=bytes(result))
    struct.pack_into("<I", result, patched.OPTIONAL_HEADER.get_field_absolute_offset("CheckSum"),
                     patched.generate_checksum())
    metadata = {"format": 1, "section_rva": section_rva,
                "section_raw_offset": raw_offset, "exports": exports, "wrappers": wrappers,
                "old_relocations": len(old_relocs), "new_relocations": len(new_relocs),
                "original_sha256": sha(data), "patched_sha256": sha(result)}
    return bytes(result), metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="633eebaf Vince.exe to patch (read only)")
    parser.add_argument("--output", type=Path, default=ROOT / "build/Vince.exe")
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve() or args.output.exists():
        parser.error("output must be a new path different from the input")
    patched, metadata = build_patch(args.input.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as stream:
        stream.write(patched)
    args.output.with_suffix(".narration.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
