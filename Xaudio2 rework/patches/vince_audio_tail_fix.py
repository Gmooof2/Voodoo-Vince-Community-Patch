"""Voodoo Vince Remastered (Steam 1.14.2.0) - streamed-audio tail fix.

Bug: the SoundStreamer feeder thread (Vince.exe 0x45D640) submits audio to
XAudio2 only in whole 132,300-byte chunks (0x204CC). For the last piece of
a non-looping file, which is smaller than a chunk, the "is a full chunk
buffered?" test at 0x45D6F5 never passes, so that piece is never
submitted. The voice runs dry, SoundStreamer's finish check (reader done and
0 buffers queued) ends the stream, and up to 2.7 s of ADPCM music
(vindeath, jazzclubjam, ...) or 0.75-3 s of PCM is lost.

Fix: if a full chunk is not available but the reader thread has finished
(streamer+0x1E) and some bytes remain in the ring buffer, submit the
remainder. Looping streams never set +0x1E, so they behave as before.

Usage:  python vince_audio_tail_fix.py --input <Vince.exe> --output <new file> [--revert]
The input is only read; the result goes to --output, which must not exist and must not be the input (no backup
file; 1.3 T5, S-vinceaudio-05; was: patch "<game folder or Vince.exe>" in place with a .audiofix-backup copy).
"""
import argparse
import hashlib
import os
import struct
import sys

IMAGE_BASE = 0x400000
SITE_VA = 0x45D6EF      # mov eax,[ebp-0C] / mov esi,[ebp+8] / cmp eax,esi / jae 45D703
CAVE_VA = 0x697804      # int3 padding (0x697802..0x69782F)
GO_VA = 0x45D703        # submit path: ebx = min(chunk, avail)
SLEEP_VA = 0x45D6F9     # push 1 / call [Sleep] / jmp loop

SITE_ORIG = bytes.fromhex("8b45f4 8b7508 3bc6 730a".replace(" ", ""))
CAVE_ORIG = b"\xCC" * 40


def rel32(src_next, dst):
    return struct.pack("<i", dst - src_next)


def build_cave():
    code = bytearray()
    va = CAVE_VA

    def emit(b):
        nonlocal va
        code.extend(b)
        va += len(b)

    emit(bytes.fromhex("8b45f4"))                 # mov eax,[ebp-0C]   ; bytes buffered
    emit(bytes.fromhex("8b7508"))                 # mov esi,[ebp+8]    ; chunk size
    emit(bytes.fromhex("3bc6"))                   # cmp eax,esi
    emit(b"\x0f\x83" + rel32(va + 6, GO_VA))      # jae GO             ; full chunk: as before
    emit(bytes.fromhex("807f1e00"))               # cmp byte [edi+1E],0 ; reader finished?
    emit(b"\x0f\x84" + rel32(va + 6, SLEEP_VA))   # je  SLEEP
    emit(bytes.fromhex("8b431c"))                 # mov eax,[ebx+1C]   ; re-read, reader is done
    emit(bytes.fromhex("85c0"))                   # test eax,eax
    emit(b"\x0f\x84" + rel32(va + 6, SLEEP_VA))   # je  SLEEP          ; nothing left
    emit(b"\xe9" + rel32(va + 5, GO_VA))          # jmp GO             ; submit the remainder
    assert len(code) == 40, len(code)
    return bytes(code)


def build_site():
    return b"\xe9" + rel32(SITE_VA + 5, CAVE_VA) + b"\x90" * 5


def va2off(pe_bytes, va):
    e_lfanew = struct.unpack_from("<I", pe_bytes, 0x3C)[0]
    nsec = struct.unpack_from("<H", pe_bytes, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", pe_bytes, e_lfanew + 20)[0]
    sec = e_lfanew + 24 + opt_size
    rva = va - IMAGE_BASE
    for i in range(nsec):
        vsz, vaddr, rawsz, rawptr = struct.unpack_from("<IIII", pe_bytes, sec + i * 40 + 8)
        if vaddr <= rva < vaddr + max(vsz, rawsz):
            return rawptr + rva - vaddr
    raise ValueError(hex(va))


def write_new(path, data):
    """Create path with data; refuses (FileExistsError) if it exists."""
    with open(path, "xb") as f:
        f.write(data)


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
    s_off, c_off = va2off(data, SITE_VA), va2off(data, CAVE_VA)
    site_new, cave_new = build_site(), build_cave()
    cur_site, cur_cave = bytes(data[s_off:s_off + 10]), bytes(data[c_off:c_off + 40])

    if revert:
        if cur_site == SITE_ORIG and cur_cave == CAVE_ORIG:
            print("Not patched; nothing to revert, nothing written.")
            return 0
        if cur_site != site_new or cur_cave != cave_new:
            print("Unexpected bytes; refusing, nothing written.")
            return 1
        data[s_off:s_off + 10], data[c_off:c_off + 40] = SITE_ORIG, CAVE_ORIG
        write_new(out, data)
        print(f"Reverted: {out}")
        return 0

    if cur_site == site_new and cur_cave == cave_new:
        print("Already patched; nothing written.")
        return 0
    if cur_site != SITE_ORIG or cur_cave != CAVE_ORIG:
        print("Vince.exe bytes don't match Steam 1.14.2.0; refusing, nothing written.")
        return 1
    data[s_off:s_off + 10], data[c_off:c_off + 40] = site_new, cave_new
    write_new(out, data)
    print(f"Patched: {out} SHA256 {hashlib.sha256(data).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
