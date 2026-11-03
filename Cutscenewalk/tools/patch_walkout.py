"""Build a patched copy of Vince.exe that restores the area-exit walk-out.

Reads the game executable, never writes it. Assembles src/walkout.s, places it
in a new section (.vwalk), and redirects two branches to it: the fade-out
branch of the load screen's Transition, and the main loop's call that deletes
the shadow-casting lights as it exits. When the section table is full, the PE
headers move down into the DOS stub to make room for the new section header.
Every patched location is checked against the expected original bytes first,
so an executable that another fix already changed there is refused.
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
SECTION_NAME = b".vwalk"

SKIP_RVA = 0x104C52              # where Transition's fade-out branch goes: snapshot + fade
# Branches redirected into the payload: RVA, stock bytes, opcode kept, payload symbol.
# Bytes after the new branch are filled with int3 (nothing jumps into them).
HOOKS = [
    (0x104B4B, "0f8501010000", "0f85", "_walkout_entry"),  # Transition: jne to the fade-out
    (0x10B78D, "e8ae88f8ff", "e8", "_teardown_gate"),      # main loop exit: call SHADOW_TEARDOWN
]
# Places in the game src/walkout.s jumps to by name: RVA and the stock bytes there.
JUMP_TARGETS = {
    "ORIGINAL_SKIP": (SKIP_RVA, "f30f10450c"),             # movss xmm0, [ebp+0xc]
}
# Game functions src/walkout.s calls by name: RVA and the start of their stock code.
GAME_CALLS = {
    "GET_SCRIPT":    (0x02D920, "558bec6aff681cdf6800"),  # SwampSceneNode::GetScript
    "BEGIN_SCENE":   (0x03FE10, "558bec83e4f851a1f053"),  # engine: clock, input and audio tick
    "GAME_FRAME":    (0x10BAA0, "558bec6aff6802ea6a00"),  # the main loop's per-frame world update
    "DEVICE_BEGIN":  (0x063EB0, "558bec83e4f883ec5480"),  # renderer: begin frame (one arg)
    "RENDER_FRAME":  (0x08E080, "558bec6aff6806126a00"),  # render all passes; arg 0: no begin/Present
    "PRESENT":       (0x064180, "558bec83e4f851568bf1"),  # renderer: end frame and Present
    "END_SCENE":     (0x03FEA0, "538bdc83ec0883e4f883"),  # engine: scripts, timers, managers
    "SHADOW_TEARDOWN": (0x094040, "558bec83ec18c745fc00"),  # delete shadow-casting lights' maps
}
# Other bytes that must still be stock: the sites and data the payload relies on.
EXPECTED = {
    0x104B40: bytes.fromhex("558bec51807d0800568bf1"),  # Transition prologue
    0x0F7FC4: bytes.fromhex("8b0151c704240000803f6a01ff5018"),  # area-load Transition(1, 1.0f)
    0x2F3448: struct.pack("<2I", 0x5D8C60, 0x434690),  # VinceWalkOut vtable
    0x2DDFEC: struct.pack("<I", 0x504120),    # VinceLoadScreen vtable slot 2: RenderLoadScene
    0x104120: bytes.fromhex("558bec6aff687fdc6a0064"),  # RenderLoadScene(alpha), thiscall
    0x104B15: bytes.fromhex("c20400"),        # ... with one argument
    0x0F7E30: bytes.fromhex("a19434b600c3"),  # LevelLoader::Instance: mov eax, [0xB63494]
    0x0FF6A3: bytes.fromhex("8a4510"),        # RequestArea stores its load-screen flag at +0x11D
    **{rva: bytes.fromhex(code) for rva, code in GAME_CALLS.values()},
}
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
    """Assemble src/walkout.s and place its code at payload_rva.

    The object must hold one code section whose only relocations are relative
    branches to the names in GAME_CALLS and JUMP_TARGETS, and image-relative
    references to its own labels. Those are resolved here: a relative branch
    within the image is position independent, and the game's .reloc needs no
    new entries.
    """
    work.mkdir(parents=True, exist_ok=True)
    assembler = LLVM_BIN / "clang.exe"
    if not assembler.is_file():
        raise ValueError(f"pinned LLVM {LLVM_VERSION} tool not found: {assembler}")
    first = (subprocess.run([str(assembler), "--version"], capture_output=True,
                            text=True).stdout.splitlines() or [""])[0]
    if f" {LLVM_VERSION} " not in first:
        raise ValueError(f"{assembler.name} must be LLVM {LLVM_VERSION}; --version reports: {first!r}")
    obj = work / "walkout.obj"
    subprocess.run([assembler, "--target=i686-pc-windows-msvc", "-c",
                    "-o", str(obj), str(ROOT / "src/walkout.s")], check=True)
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
               if section == code_sections[0] and name in {h[3] for h in HOOKS}}
    if len(entries) != len(HOOKS):
        raise ValueError(f"payload lacks some of {[h[3] for h in HOOKS]}")

    targets = {name: rva for name, (rva, _) in {**GAME_CALLS, **JUMP_TARGETS}.items()}
    for r in range(nrelocs):
        offset, index, kind = struct.unpack_from("<IIH", data, relocs + 10 * r)
        target, value, section = symbols[index]
        addend = struct.unpack_from("<i", code, offset)[0]
        if kind == IMAGE_REL_I386_REL32 and section == 0 and target in targets:
            # Relative call or branch into the game.
            struct.pack_into("<i", code, offset, targets[target] - (payload_rva + offset + 4) + addend)
        elif kind == IMAGE_REL_I386_DIR32NB and section == code_sections[0]:
            # RVA of a label in the payload (image base, constants).
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
    for rva, stock, _, name in HOOKS:
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
    for rva, stock, opcode, name in HOOKS:
        # Relative branches carry no base relocation, so rewriting them is ASLR-safe.
        length = len(stock) // 2
        branch_end = rva + len(opcode) // 2 + 4
        hook = bytes.fromhex(opcode) + struct.pack("<i", section_rva + entries[name] - branch_end)
        start = pe.get_offset_from_rva(rva)
        result[start:start + length] = hook.ljust(length, b"\xcc")
    patched = pefile.PE(data=bytes(result))
    struct.pack_into("<I", result, patched.OPTIONAL_HEADER.get_field_absolute_offset("CheckSum"),
                     patched.generate_checksum())
    metadata = {"format": 4, "section": SECTION_NAME.decode(), "payload_rva": section_rva,
                "hooks": {name: {"site_rva": rva, "target_rva": section_rva + entries[name]}
                          for rva, _, _, name in HOOKS},
                "payload_size": len(payload),
                "e_lfanew": struct.unpack_from("<I", result, 0x3C)[0],
                "input_sha256": sha(data), "output_sha256": sha(result)}
    return bytes(result), metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Vince.exe to patch (read only)")
    parser.add_argument("--output", type=Path, default=ROOT / "build/Vince.exe")
    args = parser.parse_args()
    if args.output.exists() or args.input.resolve() == args.output.resolve():
        parser.error("output must be a new path different from the input")
    patched, metadata = build_patch(args.input.read_bytes(), ROOT / "build/payload")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as stream:
        stream.write(patched)
    args.output.with_suffix(".walkout.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
