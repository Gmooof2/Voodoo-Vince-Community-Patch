"""Build an offline copy with per-invocation lightning UI-state preservation.

No input file is modified. Only the inspected Steam x86 code image is accepted;
shader/resource patches outside .text remain byte-for-byte intact.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct

import pefile

IMAGE_BASE = 0x400000
TEXT_SHA256 = "022d4f2fbac837b7a9c44b1b5731dcbdd0ad3e4572d9643b8154415ad03ed50f"
ENTRY_RVA = 0x9D608
ENTRY_CONTINUATION_RVA = 0x9D60E
EXIT_RVA = 0x9D8DC
EXIT_CONTINUATION_RVA = 0x9D8E6
SET_CONSTANT_RVA = 0x5F110
RENDERER_GLOBAL_RVA = 0x765538
ENTRY_ORIGINAL = bytes.fromhex("81ec84000000")
EXIT_ORIGINAL = bytes.fromhex("8b4df464890d00000000")
MARKER = b"VinceLightningState-v1\0"


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


class Stub:
    def __init__(self, rva):
        self.rva = rva
        self.code = bytearray()

    def emit(self, blob):
        self.code.extend(blob)

    def relative(self, opcode, target):
        self.emit(bytes([opcode]) + struct.pack("<i", target - (self.rva + len(self.code) + 5)))

    def base_in_ebx(self):
        # call next; pop ebx; sub ebx, return-RVA. No absolute VA or relocation.
        self.emit(b"\xe8\0\0\0\0\x5b")
        self.emit(b"\x81\xeb" + struct.pack("<I", self.rva + len(self.code) - 1))

    def renderer_in_ecx(self):
        self.emit(b"\x8b\x8b" + struct.pack("<I", RENDERER_GLOBAL_RVA))


def entry_stub(rva):
    s = Stub(rva)
    s.emit(bytes.fromhex("81ecd40000009c60fc"))  # extra80; pushfd/pushad/cld
    s.base_in_ebx()
    s.renderer_in_ecx()
    s.emit(bytes.fromhex("8b81bc0600008b008b7014"))  # VS constants data pointer
    s.emit(bytes.fromhex("81c6800000008dbd20ffffffb910000000f3a5"))
    # ESI advanced to data+c12. Advance another35 float4 to c47.
    s.emit(bytes.fromhex("81c630020000b904000000f3a5"))
    s.emit(bytes.fromhex("619d"))
    s.relative(0xE9, ENTRY_CONTINUATION_RVA)
    return bytes(s.code)


def exit_stub(rva):
    s = Stub(rva)
    s.emit(bytes.fromhex("9c60fc"))
    s.base_in_ebx()
    # SetConstants(VS=0, start=8, savedMatrix, count=4), callee pops16.
    s.emit(bytes.fromhex("6a048d8520ffffff506a086a00"))
    s.renderer_in_ecx()
    s.relative(0xE8, SET_CONSTANT_RVA)
    s.emit(bytes.fromhex("6a018d8560ffffff506a2f6a00"))
    s.renderer_in_ecx()
    s.relative(0xE8, SET_CONSTANT_RVA)
    s.emit(bytes.fromhex("619d"))
    s.emit(EXIT_ORIGINAL)
    s.relative(0xE9, EXIT_CONTINUATION_RVA)
    return bytes(s.code)


def build_patch(data, identity=True):
    """Return (patched_bytes, metadata). Reject unsupported/already patched inputs.

    identity=False (AIO build, phase 1.4) skips only the TimeDateStamp, SizeOfImage
    and .text sha256 pins; the hook bytes are still compared before writing.
    """
    pe = pefile.PE(data=data)
    if MARKER in data or any(s.Name.rstrip(b"\0") == b".vlight" for s in pe.sections):
        raise ValueError("already contains the lightning state patch")
    if (pe.FILE_HEADER.Machine != 0x14C or pe.OPTIONAL_HEADER.Magic != 0x10B
            or pe.OPTIONAL_HEADER.ImageBase != IMAGE_BASE
            or identity and (pe.FILE_HEADER.TimeDateStamp != 0x5996A9BB
                             or pe.OPTIONAL_HEADER.SizeOfImage != 0x87A000)):
        raise ValueError("unsupported executable identity")
    text = next((s for s in pe.sections if s.Name.rstrip(b"\0") == b".text"), None)
    if text is None or identity and hashlib.sha256(text.get_data()).hexdigest() != TEXT_SHA256:
        raise ValueError("unsupported executable code (.text checksum differs)")
    if pe.OPTIONAL_HEADER.DATA_DIRECTORY[4].VirtualAddress:
        raise ValueError("signed executables are unsupported")
    for rva, original in [(ENTRY_RVA, ENTRY_ORIGINAL), (EXIT_RVA, EXIT_ORIGINAL)]:
        offset = pe.get_offset_from_rva(rva)
        if data[offset:offset + len(original)] != original:
            raise ValueError(f"unexpected hook bytes at RVA {rva:X}")
    # Neither detour may overwrite a relocation, even in a changed PE layout.
    pe.parse_data_directories(directories=[5])
    for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for reloc in block.entries:
            if reloc.type and any(r <= reloc.rva < r + n for r, n in [(ENTRY_RVA, 6), (EXIT_RVA, 10)]):
                raise ValueError("hook overlaps an existing relocation")
    section_header = pe.sections[-1].get_file_offset() + 40
    first_raw = min(s.PointerToRawData for s in pe.sections if s.SizeOfRawData)
    if section_header + 40 > min(first_raw, pe.OPTIONAL_HEADER.SizeOfHeaders):
        raise ValueError("insufficient PE section-header space")
    if any(data[section_header:section_header + 40]):
        raise ValueError("new section header would overwrite existing data")
    section_alignment = pe.OPTIONAL_HEADER.SectionAlignment
    file_alignment = pe.OPTIONAL_HEADER.FileAlignment
    section_rva = align(max(s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)
                            for s in pe.sections), section_alignment)
    raw_offset = align(len(data), file_alignment)
    entry_rva = section_rva
    enter = entry_stub(entry_rva)
    exit_rva = align(entry_rva + len(enter), 16)
    leave = exit_stub(exit_rva)
    payload = enter + b"\xcc" * (exit_rva - entry_rva - len(enter)) + leave + MARKER
    raw_size = align(len(payload), file_alignment)
    result = bytearray(data)
    result.extend(b"\0" * (raw_offset - len(result)))
    result.extend(payload + b"\0" * (raw_size - len(payload)))
    header = struct.pack("<8sIIIIIIHHI", b".vlight\0", len(payload), section_rva,
                         raw_size, raw_offset, 0, 0, 0, 0, 0x60000020)
    result[section_header:section_header + 40] = header
    struct.pack_into("<H", result, pe.FILE_HEADER.get_field_absolute_offset("NumberOfSections"),
                     pe.FILE_HEADER.NumberOfSections + 1)
    struct.pack_into("<I", result, pe.OPTIONAL_HEADER.get_field_absolute_offset("SizeOfImage"),
                     align(section_rva + len(payload), section_alignment))
    struct.pack_into("<I", result, pe.OPTIONAL_HEADER.get_field_absolute_offset("SizeOfCode"),
                     pe.OPTIONAL_HEADER.SizeOfCode + raw_size)
    for rva, target, size in [(ENTRY_RVA, entry_rva, 6), (EXIT_RVA, exit_rva, 10)]:
        offset = pe.get_offset_from_rva(rva)
        result[offset:offset + size] = b"\xe9" + struct.pack("<i", target - rva - 5) + b"\x90" * (size - 5)
    # Preserve relocation directory, imports, subsystem and all security flags.
    # Recompute the advisory checksum after finalizing headers and code.
    patched = pefile.PE(data=bytes(result))
    struct.pack_into("<I", result, patched.OPTIONAL_HEADER.get_field_absolute_offset("CheckSum"),
                     patched.generate_checksum())
    metadata = {
        "entry_stub_rva": entry_rva, "exit_stub_rva": exit_rva,
        "entry_stub_size": len(enter), "exit_stub_size": len(leave),
        "entry_continuation_rva": ENTRY_CONTINUATION_RVA,
        "exit_continuation_rva": EXIT_CONTINUATION_RVA,
        "patch_sites": [ENTRY_RVA, EXIT_RVA],
        "original_sha256": hashlib.sha256(data).hexdigest(),
        "patched_sha256": hashlib.sha256(result).hexdigest(),
        "section_rva": section_rva, "section_raw_offset": raw_offset,
    }
    return bytes(result), metadata


def build_patched(data):
    return build_patch(data)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="Vince.exe to read (never modified)")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "build" / "Vince.exe")
    args = parser.parse_args()
    if args.output.resolve() == args.input.resolve():
        parser.error("output must differ from input")
    if args.output.exists():
        parser.error("output already exists; choose a fresh path")
    data, metadata = build_patch(args.input.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as stream:
        stream.write(data)
    receipt = args.output.with_suffix(args.output.suffix + ".lightning.json")
    with receipt.open("x", encoding="utf-8") as stream:
        json.dump(metadata, stream, indent=2)
        stream.write("\n")
    print(f"Created {args.output}")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
