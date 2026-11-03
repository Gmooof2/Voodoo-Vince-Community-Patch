"""Voodoo Vince Remastered (Steam 1.14.2.0) - streamed-audio end fix (Patch D).

Bug: the main-thread end check (Vince.exe 0x45CA68) ends a non-looping stream when
the reader is at EOF and XAudio2 has 0 buffers queued, but the feeder thread may
still hold data on its way to XAudio2: before its first take (ring bytes
available > 0) or between a take and its SubmitSourceBuffer. A short jingle is
then cut or silent.

Fix: the feeder marks streamer+0x23 = 1 before each take (0x45D703) and 0 after
each submit (0x45D769); the end check also requires (ring taken total == 0, or
ring available == 0 and +0x23 == 0). Needs patch B (vince_audio_tail_fix.py):
without it a sub-chunk tail is never taken and the stream would never end.
Design: docs/ops/patchset.md `## Patch D`.

Usage:  python vince_audio_end_fix.py --input <Vince.exe> --output <new file> [--revert]
The input is only read; the result goes to --output, which must not exist and must not be the input (no backup
file; 1.3 T5, S-vinceaudio-05; was: patch "<game folder or Vince.exe>" in place with a .endfix-backup copy).
"""
import argparse
import hashlib
import os
import runpy
import struct
import sys

TAIL = runpy.run_path(os.path.join(os.path.dirname(os.path.abspath(__file__)), "vince_audio_tail_fix.py"))
IMAGE_BASE = 0x400000
MAIN_SITE_VA = 0x45CA68     # cmp byte [esi+1E],0 / je 45CA80
MAIN_GOON_VA = 0x45CA6E     # the game's GetState (BuffersQueued) check
MAIN_BACK_VA = 0x45CA80     # epilogue: stream keeps playing
MAIN_CAVE_VA = 0x587B58     # int3 padding (0x587B58..0x587B7F)
GO_SITE_VA = 0x45D703       # GO: cmp eax,esi / mov ebx,esi / cmovb ebx,eax
GO_BACK_VA = 0x45D70A
GO_CAVE_VA = 0x540F91       # int3 padding (0x540F91..0x540FAF)
POST_SITE_VA = 0x45D769     # mov byte [edi+22],1 / jmp 45D695
LOOP_VA = 0x45D695          # consumer loop top
POST_CAVE_VA = 0x540FA1

MAIN_SITE_ORIG = bytes.fromhex("807e1e007412")
GO_SITE_ORIG = bytes.fromhex("3bc68bde0f42d8")
POST_SITE_ORIG = bytes.fromhex("c6472201e923ffffff")
MAIN_CAVE_ORIG = b"\xCC" * 40
GO_CAVE_ORIG = b"\xCC" * 16
POST_CAVE_ORIG = b"\xCC" * 13

# design hex, docs/ops/patchset.md `### Sites and caves`
DESIGN = {
    "main site": "e9ebb0120090", "go site": "e989380e009090", "post site": "e933380e0090909090",
    "main cave": "807e1e00741d8b86fc01000083781c007511837814007406807e23007505e9f34eedffe9004fedff",
    "go cave": "c64723013bc68bde0f42d8e969c7f1ff", "post cave": "c6472201c6472300e9e7c6f1ff",
}


def rel32(src_next, dst):
    return struct.pack("<i", dst - src_next)


def jmp(src, dst):
    return b"\xe9" + rel32(src + 5, dst)


def build_main_cave():
    code = bytearray()
    shorts = []                                      # (offset of rel8, label)

    def jcc(op, label):
        code.extend(bytes([op, 0]))
        shorts.append((len(code) - 1, label))

    code.extend(bytes.fromhex("807e1e00"))           # cmp byte [esi+1E],0  ; reader at EOF?
    jcc(0x74, "BACK")                                # je  BACK
    code.extend(bytes.fromhex("8b86fc010000"))       # mov eax,[esi+1FC]    ; ring
    code.extend(bytes.fromhex("83781c00"))           # cmp dword [eax+1C],0 ; bytes available
    jcc(0x75, "BACK")                                # jne BACK
    code.extend(bytes.fromhex("83781400"))           # cmp dword [eax+14],0 ; bytes taken total
    jcc(0x74, "GOON")                                # je  GOON             ; nothing ever written
    code.extend(bytes.fromhex("807e2300"))           # cmp byte [esi+23],0  ; take-to-submit window?
    jcc(0x75, "BACK")                                # jne BACK
    labels = {"GOON": len(code)}
    code.extend(jmp(MAIN_CAVE_VA + len(code), MAIN_GOON_VA))   # GOON: jmp 45CA6E
    labels["BACK"] = len(code)
    code.extend(jmp(MAIN_CAVE_VA + len(code), MAIN_BACK_VA))   # BACK: jmp 45CA80
    for at, label in shorts:
        disp = labels[label] - (at + 1)
        assert 0 <= disp < 0x80, (label, disp)
        code[at] = disp
    assert len(code) == len(MAIN_CAVE_ORIG), len(code)
    return bytes(code)


def build_go_cave():
    code = bytes.fromhex("c6472301") + GO_SITE_ORIG  # mov byte [edi+23],1 ; then the replaced GO code
    code += jmp(GO_CAVE_VA + len(code), GO_BACK_VA)  # jmp 45D70A
    assert len(code) == len(GO_CAVE_ORIG), len(code)
    return code


def build_post_cave():
    code = POST_SITE_ORIG[:4] + bytes.fromhex("c6472300")   # mov byte [edi+22],1 / mov byte [edi+23],0
    code += jmp(POST_CAVE_VA + len(code), LOOP_VA)           # jmp 45D695
    assert len(code) == len(POST_CAVE_ORIG), len(code)
    return code


def build_site(site_va, cave_va, length):
    return jmp(site_va, cave_va) + b"\x90" * (length - 5)


def sites():
    """[(label, va, orig, new)] in spec order: main site, main cave, go site, go cave, post site, post cave."""
    out = [
        ("main site", MAIN_SITE_VA, MAIN_SITE_ORIG, build_site(MAIN_SITE_VA, MAIN_CAVE_VA, len(MAIN_SITE_ORIG))),
        ("main cave", MAIN_CAVE_VA, MAIN_CAVE_ORIG, build_main_cave()),
        ("go site", GO_SITE_VA, GO_SITE_ORIG, build_site(GO_SITE_VA, GO_CAVE_VA, len(GO_SITE_ORIG))),
        ("go cave", GO_CAVE_VA, GO_CAVE_ORIG, build_go_cave()),
        ("post site", POST_SITE_VA, POST_SITE_ORIG, build_site(POST_SITE_VA, POST_CAVE_VA, len(POST_SITE_ORIG))),
        ("post cave", POST_CAVE_VA, POST_CAVE_ORIG, build_post_cave()),
    ]
    for label, _, orig, new in out:
        assert len(new) == len(orig) and new.hex() == DESIGN[label], label
    return out


va2off = TAIL["va2off"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", required=True, help="Vince.exe to read (never written)")
    parser.add_argument("--output", required=True, help="new file for the result (must not exist)")
    parser.add_argument("--revert", action="store_true")
    args = parser.parse_args()
    path, out = args.input, args.output
    same = os.path.normcase(os.path.realpath(out)) == os.path.normcase(os.path.realpath(path))
    if same or os.path.exists(out):
        parser.error("--output must be a new file, not the input; nothing written")
    revert = args.revert
    data = bytearray(open(path, "rb").read())
    table = [(va2off(data, va), orig, new) for _, va, orig, new in sites()]
    cur = [bytes(data[o:o + len(orig)]) for o, orig, _ in table]
    original = all(c == orig for c, (_, orig, _) in zip(cur, table))
    patched = all(c == new for c, (_, _, new) in zip(cur, table))

    if revert:
        if original:
            print("Not patched; nothing to revert, nothing written.")
            return 0
        if not patched:
            print("Unexpected bytes; refusing, nothing written.")
            return 1
        for o, orig, _ in table:
            data[o:o + len(orig)] = orig
        TAIL["write_new"](out, data)
        print(f"Reverted: {out}")
        return 0

    if patched:
        print("Already patched; nothing written.")
        return 0
    if not original:
        print("Vince.exe bytes don't match Steam 1.14.2.0; refusing, nothing written.")
        return 1
    b_site, b_cave = va2off(data, TAIL["SITE_VA"]), va2off(data, TAIL["CAVE_VA"])
    if (bytes(data[b_site:b_site + len(TAIL["SITE_ORIG"])]) != TAIL["build_site"]()
            or bytes(data[b_cave:b_cave + len(TAIL["CAVE_ORIG"])]) != TAIL["build_cave"]()):
        print("The tail fix (vince_audio_tail_fix.py) is not applied; apply it first. Refusing, nothing written.")
        return 1
    for o, orig, new in table:
        data[o:o + len(orig)] = new
    TAIL["write_new"](out, data)
    print(f"Patched: {out} SHA256 {hashlib.sha256(data).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
