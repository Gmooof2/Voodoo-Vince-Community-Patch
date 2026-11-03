"""Build a patched copy of Vince.exe that no longer crashes when a cutscene is skipped.

Reads the game executable, never writes it. Assembles src/cutscenefix.s,
places it in a new section (.vcsfix), and redirects three places to it: the
script dispatcher's call of a script's slot 0x18, the prologue of
CutsceneAnimationScript slot 0x28, and the end of CutsceneDriver's destructor.
Every patched location and everything the payload relies on is checked against
the expected original bytes first, so an executable that another fix already
changed there is refused.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
import pefile

BASE = 0x400000
LLVM_BIN = Path("C:/Program Files/LLVM/bin")
LLVM_VERSION = "23.1.3"
TIMESTAMP = 0x5996A9BB
SECTION_NAME = b".vcsfix"

# Places redirected into the payload with a jmp: RVA, stock bytes, payload symbol.
# Bytes after the new jmp are filled with int3 (nothing jumps into them).
HOOKS = [
    (0x02E1A8, "8b55f88b028b4df88b5018ffd2", "_dispatch_guard"),  # dispatcher: call script slot 0x18
    (0x0D1120, "558bec8bd1", "_anim_driver_guard"),               # CutsceneAnimationScript slot 0x28
    (0x0D1039, "8bcfe860ccf5ff", "_driver_dtor_tail"),            # CutsceneDriver dtor: base dtor call
]
# Places in the game src/cutscenefix.s jumps to or calls by name: RVA and the stock bytes there.
JUMP_TARGETS = {
    "DISPATCH_NEXT":    (0x02E266, "e9b7feffff"),           # the dispatcher's loop, after any case
    "DRIVER_CALL":      (0x0D1125, "8b4a4085c97426ff7514"),  # slot 0x28 after its prologue
    "DTOR_RESUME":      (0x0D1040, "8b4df464890d00000000"),  # the destructor's epilogue
    "SCRIPT_BASE_DTOR": (0x02DCA0, "558bec6aff68f7c66800"),  # Script's destructor
}
# Other bytes that must still be stock: code and data the payload relies on.
EXPECTED = {
    0x02E1A1: bytes.fromhex("ff248d70e24200"),               # the dispatcher's switch on 0x42E270
    0x0D1100: bytes.fromhex("8bd18b4a4085c974088b01ff7244ff5068c3"),  # slot 0x18: driver->vfunc[0x68]
    0x0D112A: bytes.fromhex("7426"),                          # slot 0x28 skips on a null driver ...
    0x0D114C: bytes.fromhex("ff7244ff506c5dc21000"),          # ... else driver->vfunc[0x6C], ret 16
    0x0D1C7F: bytes.fromhex("c706e0786d00897e40894e44"),      # constructor: vtable, driver at +0x40
    0x0D0FDD: bytes.fromhex("c7074c796d00"),                  # CutsceneDriver dtor sets its vtable
    0x2D78E0 + 0x18: struct.pack("<I", 0x4D1100),            # CutsceneAnimationScript slot 0x18
    0x2D78E0 + 0x28: struct.pack("<I", 0x4D1120),            # ... and slot 0x28
    0x75AA84 + 8: b".?AVCutsceneDriver@@\0",                  # type descriptor's name
}
RDATA = (0x2D6000, 0x74E000)  # must match RVA_RDATA_START/END in src/cutscenefix.s
IMAGE_REL_I386_DIR32NB = 0x07
IMAGE_REL_I386_REL32 = 0x14


def sha(data):
    return hashlib.sha256(data).hexdigest()


def align(n, a):
    return (n + a - 1) // a * a


def coff_symbol_name(raw, strings):
    if raw[:4] == b"\0\0\0\0":
        offset = struct.unpack_from("<I", raw, 4)[0]
        return strings[offset:strings.index(b"\0", offset)].decode()
    return raw.rstrip(b"\0").decode()


def compile_payload(payload_rva, work):
    """Assemble src/cutscenefix.s and place its code at payload_rva.

    The object must hold one code section whose only relocations are relative
    branches to the names in JUMP_TARGETS, and image-relative references to its
    own labels. Those are resolved here: a relative branch within the image is
    position independent, and the game's .reloc needs no new entries.
    """
    work.mkdir(parents=True, exist_ok=True)
    assembler = LLVM_BIN / "clang.exe"
    if not assembler.is_file():
        raise ValueError(f"pinned LLVM {LLVM_VERSION} tool not found: {assembler}")
    first = (subprocess.run([str(assembler), "--version"], capture_output=True,
                            text=True).stdout.splitlines() or [""])[0]
    if f" {LLVM_VERSION} " not in first:
        raise ValueError(f"{assembler.name} must be LLVM {LLVM_VERSION}; --version reports: {first!r}")
    obj = work / "cutscenefix.obj"
    subprocess.run([assembler, "--target=i686-pc-windows-msvc", "-c",
                    "-o", str(obj), str(ROOT / "src/cutscenefix.s")], check=True)
    data = obj.read_bytes()
    machine, nsections, _, symtab, nsyms, optsize, _ = struct.unpack_from("<HHIIIHH", data, 0)
    strings = data[symtab + 18 * nsyms:]
    sections = {}
    for i in range(nsections):
        name, _, _, size, raw, relocs, _, nrelocs = struct.unpack_from(
            "<8sIIIIIIH", data, 20 + optsize + 40 * i)
        if size:
            sections[i + 1] = (name.rstrip(b"\0"), size, raw, relocs, nrelocs)
    code_sections = [i for i, s in sections.items() if s[0] == b".text"]
    if machine != 0x14C or len(sections) != 1 or len(code_sections) != 1:
        raise ValueError(f"payload must be one i386 .text section, got {[s[0] for s in sections.values()]}")
    name, size, raw, relocs, nrelocs = sections[code_sections[0]]
    code = bytearray(data[raw:raw + size])

    symbols, i = {}, 0
    while i < nsyms:
        raw_name, value, section, _, _, aux = struct.unpack_from("<8sIhHBB", data, symtab + 18 * i)
        symbols[i] = (coff_symbol_name(raw_name, strings), value, section)
        i += 1 + aux
    entries = {name: value for name, value, section in symbols.values()
               if section == code_sections[0] and name in {h[2] for h in HOOKS}}
    if len(entries) != len(HOOKS):
        raise ValueError(f"payload lacks some of {[h[2] for h in HOOKS]}")

    targets = {name: rva for name, (rva, _) in JUMP_TARGETS.items()}
    for r in range(nrelocs):
        offset, index, kind = struct.unpack_from("<IIH", data, relocs + 10 * r)
        target, value, section = symbols[index]
        addend = struct.unpack_from("<i", code, offset)[0]
        if kind == IMAGE_REL_I386_REL32 and section == 0 and target in targets:
            # Relative call or branch into the game.
            struct.pack_into("<i", code, offset, targets[target] - (payload_rva + offset + 4) + addend)
        elif kind == IMAGE_REL_I386_DIR32NB and section == code_sections[0]:
            # RVA of a label in the payload (to find the image base).
            struct.pack_into("<I", code, offset, payload_rva + value + addend)
        else:
            raise ValueError(f"unsupported payload reference to {target!r} (type {kind:#x})")
    return bytes(code), entries


def add_section(result, pe):
    """Append an empty executable section header to result; return its RVA.

    The section table must end before SizeOfHeaders. If the new header does not
    fit, the NT headers and section table move down to right after the DOS
    header (offset 0x40), over the DOS stub and the linker's Rich header,
    which Windows does not use. finish_section sets the sizes and adds the data.
    """
    if any(s.Name.rstrip(b"\0") == SECTION_NAME for s in pe.sections):
        raise ValueError(f"{SECTION_NAME.decode()} section already present (already patched?)")
    opt = pe.OPTIONAL_HEADER
    for d in opt.DATA_DIRECTORY:
        if d.VirtualAddress and d.VirtualAddress < opt.SizeOfHeaders:
            raise ValueError(f"data directory {d.name} lies in the headers")
    last = pe.sections[-1]
    file_end = last.PointerToRawData + last.SizeOfRawData
    if len(result) != file_end:
        raise ValueError("data after the last section (overlay) is unsupported")
    lfanew = pe.DOS_HEADER.e_lfanew
    count = pe.FILE_HEADER.NumberOfSections
    headers = 4 + 20 + pe.FILE_HEADER.SizeOfOptionalHeader + 40 * count
    if lfanew + headers + 40 > opt.SizeOfHeaders:
        if 0x40 + headers + 40 > opt.SizeOfHeaders:
            raise ValueError("no room for another section header")
        block = bytes(result[lfanew:lfanew + headers])
        result[0x40:opt.SizeOfHeaders] = bytes(opt.SizeOfHeaders - 0x40)
        result[0x40:0x40 + headers] = block
        struct.pack_into("<I", result, 0x3C, 0x40)
        lfanew = 0x40
    elif any(result[lfanew + headers:lfanew + headers + 40]):
        raise ValueError("bytes after the section table are in use")
    rva = align(last.VirtualAddress + last.Misc_VirtualSize, opt.SectionAlignment)
    struct.pack_into("<8sIIIIIIHHI", result, lfanew + headers, SECTION_NAME, 0, rva, 0, file_end,
                     0, 0, 0, 0, 0x60000020)       # code, execute, read
    struct.pack_into("<H", result, lfanew + 6, count + 1)
    return rva


def finish_section(result, payload):
    """Size the section add_section created (the last one) and append payload."""
    lfanew = struct.unpack_from("<I", result, 0x3C)[0]
    count, optsize = struct.unpack_from("<H", result, lfanew + 6)[0], struct.unpack_from("<H", result, lfanew + 20)[0]
    opt = lfanew + 24
    section_alignment, file_alignment = struct.unpack_from("<II", result, opt + 32)
    header = opt + optsize + 40 * (count - 1)
    rva, raw = struct.unpack_from("<I", result, header + 12)[0], struct.unpack_from("<I", result, header + 20)[0]
    raw_size = align(len(payload), file_alignment)
    struct.pack_into("<I", result, header + 8, len(payload))
    struct.pack_into("<I", result, header + 16, raw_size)
    result[raw:] = payload.ljust(raw_size, b"\xcc")
    size_of_code = struct.unpack_from("<I", result, opt + 4)[0]
    struct.pack_into("<I", result, opt + 4, size_of_code + raw_size)
    struct.pack_into("<I", result, opt + 56, align(rva + len(payload), section_alignment))


def build_patch(data, work, identity=True):
    # identity=False (AIO chain): skip only the TimeDateStamp; every site compare stays
    pe = pefile.PE(data=data)
    if (pe.FILE_HEADER.Machine != 0x14C or pe.OPTIONAL_HEADER.ImageBase != BASE
            or identity and pe.FILE_HEADER.TimeDateStamp != TIMESTAMP):
        raise ValueError("unsupported executable identity")
    if pe.OPTIONAL_HEADER.DATA_DIRECTORY[4].VirtualAddress:
        raise ValueError("signed executable is unsupported")
    rdata = [s for s in pe.sections if s.Name.rstrip(b"\0") == b".rdata"]
    if len(rdata) != 1 or (rdata[0].VirtualAddress, align(rdata[0].VirtualAddress + rdata[0].Misc_VirtualSize,
                                                          0x1000)) != RDATA:
        raise ValueError(".rdata is not where src/cutscenefix.s expects it")
    for rva, stock, name in HOOKS:
        if pe.get_data(rva, len(stock) // 2) != bytes.fromhex(stock):
            raise ValueError(f"hook site {rva:#x} for {name} is not stock (another fix changed it?)")
    for name, (rva, code) in JUMP_TARGETS.items():
        if pe.get_data(rva, len(code) // 2) != bytes.fromhex(code):
            raise ValueError(f"jump target {name} at {rva:#x} is not stock")
    for rva, expected in EXPECTED.items():
        if pe.get_data(rva, len(expected)) != expected:
            raise ValueError(f"unexpected code or data at RVA {rva:#x}")

    result = bytearray(data)
    section_rva = add_section(result, pe)
    payload, entries = compile_payload(section_rva, work)
    finish_section(result, payload)
    for rva, stock, name in HOOKS:
        # Relative jumps carry no base relocation, so rewriting them is ASLR-safe.
        length = len(stock) // 2
        hook = b"\xe9" + struct.pack("<i", section_rva + entries[name] - (rva + 5))
        start = pe.get_offset_from_rva(rva)
        result[start:start + length] = hook.ljust(length, b"\xcc")
    patched = pefile.PE(data=bytes(result))
    struct.pack_into("<I", result, patched.OPTIONAL_HEADER.get_field_absolute_offset("CheckSum"),
                     patched.generate_checksum())
    metadata = {"format": 1, "section": SECTION_NAME.decode(), "payload_rva": section_rva,
                "hooks": {name: {"site_rva": rva, "target_rva": section_rva + entries[name]}
                          for rva, _, name in HOOKS},
                "payload_size": len(payload),
                "input_sha256": sha(data), "output_sha256": sha(result)}
    return bytes(result), metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/Vince.exe")
    args = parser.parse_args()
    if args.output.exists() or args.input.resolve() == args.output.resolve():
        parser.error("output must be a new path different from the input")
    patched, metadata = build_patch(args.input.read_bytes(), ROOT / "build/payload")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as stream:
        stream.write(patched)
    args.output.with_suffix(".cutscenefix.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
