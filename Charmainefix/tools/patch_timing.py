"""Start Main Tunnel's own entrance narration 1.5 seconds earlier.

The existing shared completion patch stays intact. A small position-independent
detour changes only the timer for charmaine_r_mt_01 from 2.0 to 0.5 seconds.
Version 2 also clears Vince's hint quiet-time cooldown when that narration is
enqueued; otherwise the queue holds it until the 3.5-second cooldown expires.
Version 3 keeps that and requests the narration at 1.5 seconds instead.
Builds an offline copy and never overwrites the input executable.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import patch_narration as common
import pefile

INPUT_SHA256 = "8a69d711d0ac81e202bf143238103764478ca1664f1a21c3a3eee2fbf7c12f9f"
HOOK_RVA = 0xF2540
CONTINUATION_RVA = 0xF2547
ORIGINAL = bytes.fromhex("c7042400000040")
MARKER = b"VinceNarrationStart-MT-0.5-v1\0"
MARKER_V2 = b"VinceNarrationStart-MT-0.5-v2\0"
MARKER_V3 = b"VinceNarrationStart-MT-1.5-v3\0"
MARKERS = {1: MARKER, 2: MARKER_V2, 3: MARKER_V3}
DELAYS = {1: 0.5, 2: 0.5, 3: 1.5}
NAME = b"charmaine_r_mt_01"
# Timer handler 0x4F2580: ECX = [[0xB631EC]+0x1C] Vince, ESI = flythrough.
ENQUEUE_CALL_RVA = 0xF25D8
ENQUEUE_RVA = 0x124BA0  # Vince::PlayHint(name, subtitle), thiscall
COOLDOWN = 0x474  # Vince hint quiet time remaining, float seconds


def name_check(code):
    """Append the exact-name test on ESI's narration; return failure branches."""
    branches = []

    def fail(opcode):
        code.extend([opcode, 0])
        branches.append(len(code) - 1)

    code += bytes.fromhex("837e6811")  # narration string length ==17
    fail(0x75)
    code += bytes.fromhex("837e6c11")  # valid heap capacity >=17
    fail(0x72)
    code += bytes.fromhex("8b465885c0")  # name pointer, guard null
    fail(0x74)
    for offset in range(0, 16, 4):
        if offset:
            code += bytes([0x81, 0x78, offset]) + NAME[offset:offset + 4]
        else:
            code += b"\x81\x38" + NAME[:4]
        fail(0x75)
    code += bytes.fromhex("80781031")  # final '1'; length check excludes suffixes
    fail(0x75)
    return branches


def resolve(code, branches):
    end = len(code)
    for operand in branches:
        distance = end - operand - 1
        if not 0 <= distance <= 127:
            raise ValueError("short branch exceeded its range")
        code[operand] = distance


def timing_stub(rva, delay=0.5):
    code = bytearray.fromhex("9c50")  # save flags and EAX
    # The original delay slot is now 8 bytes above ESP. Default remains2.0f.
    code += bytes.fromhex("c744240800000040")
    branches = name_check(code)
    code += b"\xc7\x44\x24\x08" + struct.pack("<f", delay)  # requested Main Tunnel delay
    resolve(code, branches)
    code += bytes.fromhex("589d")
    code += b"\xe9" + struct.pack("<i", CONTINUATION_RVA - (rva + len(code) + 5))
    return bytes(code)


def cooldown_stub(rva):
    """Called instead of PlayHint; clears the quiet time only for this narration."""
    code = bytearray.fromhex("9c50")  # save flags and EAX
    branches = name_check(code)
    code += bytes.fromhex("85c9")  # Vince pointer, guard null
    code.extend([0x74, 0])
    branches.append(len(code) - 1)
    # Signed integer order matches float order for positive values, so zero,
    # negative and negative-NaN cooldowns stay untouched.
    code += b"\x83\xb9" + struct.pack("<I", COOLDOWN) + b"\x00"
    code.extend([0x7e, 0])
    branches.append(len(code) - 1)
    code += b"\xc7\x81" + struct.pack("<II", COOLDOWN, 0)
    resolve(code, branches)
    code += bytes.fromhex("589d")
    # Tail jump: the caller's return address and both arguments stay in place.
    code += b"\xe9" + struct.pack("<i", ENQUEUE_RVA - (rva + len(code) + 5))
    return bytes(code)


def build_patch(data, version=3, identity=True):
    # identity=False (AIO chain): skip only the whole-file pin; every site compare stays
    if version not in MARKERS:
        raise ValueError("unknown timing patch version")
    if any(marker in data for marker in MARKERS.values()):
        raise ValueError("already contains the earlier Main Tunnel narration timing")
    if identity and common.sha(data) != INPUT_SHA256:
        raise ValueError("unsupported executable: expected the verified narration-wait build")
    pe = pefile.PE(data=data)
    if pe.get_data(HOOK_RVA, 7) != ORIGINAL:
        raise ValueError("unexpected narration timer instruction")
    enqueue_call = b"\xe8" + struct.pack("<i", ENQUEUE_RVA - ENQUEUE_CALL_RVA - 5)
    if pe.get_data(ENQUEUE_CALL_RVA, 5) != enqueue_call:
        raise ValueError("unexpected narration enqueue call")
    if any(HOOK_RVA <= rva < CONTINUATION_RVA
           or ENQUEUE_CALL_RVA - 3 <= rva < ENQUEUE_CALL_RVA + 5
           for rva, kind in common.relocations(pe)):
        raise ValueError("timing hook overlaps an existing relocation")
    section = pe.sections[-1]
    if section.Name.rstrip(b"\0") != b".vnrfix" or section.Characteristics != 0x60000020:
        raise ValueError("missing expected executable narration section")
    start = common.align(section.Misc_VirtualSize, 16)
    stub_rva = section.VirtualAddress + start
    code = timing_stub(stub_rva, DELAYS[version])
    cooldown_rva = stub_rva + len(code)
    if version >= 2:
        code += cooldown_stub(cooldown_rva)
    payload = code + MARKERS[version]
    end = start + len(payload)
    raw = section.get_data()
    if end > section.SizeOfRawData or any(raw[section.Misc_VirtualSize:]):
        raise ValueError("insufficient unused narration section padding")
    if common.align(section.VirtualAddress + end, pe.OPTIONAL_HEADER.SectionAlignment) != pe.OPTIONAL_HEADER.SizeOfImage:
        raise ValueError("timing stub would change the mapped image size")
    result = bytearray(data)
    offset = section.PointerToRawData + start
    result[offset:offset + len(payload)] = payload
    struct.pack_into("<I", result, section.get_field_absolute_offset("Misc_VirtualSize"), end)
    hook = pe.get_offset_from_rva(HOOK_RVA)
    result[hook:hook + 7] = b"\xe9" + struct.pack("<i", stub_rva - HOOK_RVA - 5) + b"\x90\x90"
    if version >= 2:
        site = pe.get_offset_from_rva(ENQUEUE_CALL_RVA)
        result[site:site + 5] = b"\xe8" + struct.pack("<i", cooldown_rva - ENQUEUE_CALL_RVA - 5)
    patched = pefile.PE(data=bytes(result))
    struct.pack_into("<I", result, patched.OPTIONAL_HEADER.get_field_absolute_offset("CheckSum"),
                     patched.generate_checksum())
    metadata = {"format": 1, "original_sha256": common.sha(data),
                "patched_sha256": common.sha(result), "stub_rva": stub_rva,
                "stub_size": len(code), "hook_rva": HOOK_RVA,
                "continuation_rva": CONTINUATION_RVA, "section_virtual_size": end,
                "name": NAME.decode(), "old_delay_seconds": 2.0, "new_delay_seconds": DELAYS[version]}
    if version >= 2:
        metadata.update({"version": version,"cooldown_stub_rva": cooldown_rva,
                         "enqueue_call_rva": ENQUEUE_CALL_RVA, "cooldown_cleared": True})
    return bytes(result), metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="8a69d711 narration-patched Vince.exe (read only)")
    parser.add_argument("--version", type=int, choices=sorted(MARKERS), default=3)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.output is None:
        args.output = ROOT / ("build/timing/Vince.exe" if args.version == 1
                              else f"build/timing-v{args.version}/Vince.exe")
    if args.output.exists() or args.output.resolve() == args.input.resolve():
        parser.error("output must be a fresh path different from input")
    patched, metadata = build_patch(args.input.read_bytes(), args.version)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as stream:
        stream.write(patched)
    args.output.with_suffix(".timing.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
