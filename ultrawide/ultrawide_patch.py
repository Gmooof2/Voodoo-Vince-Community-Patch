"""Voodoo Vince Remastered - ultrawide (21:9 / 32:9) patch for the Steam build 1.14.2.0.

Usage (run from anywhere, Python 3, no extra modules):
    python ultrawide_patch.py            apply the patch
    python ultrawide_patch.py status     show whether each part is applied
    python ultrawide_patch.py uninstall  put the original bytes back (other patches are kept)
    python ultrawide_patch.py dryrun     show what would change (disassembly if capstone is installed)

Works for any aspect ratio wider than 16:9; 16:9 and narrower screens behave exactly as before.

What it changes in Vince.exe (all code is position independent, the game uses ASLR):
  1. Display::UpdateViewport (0x467290): wider than 16:9 no longer pillarboxes to a 16:9 box,
     the 3D scene uses the whole screen.
  2. Camera constructor (0x44B370): the camera aspect comes from the real viewport size
     instead of the hardcoded virtual 1920x1080.
  3. Camera projection (0x44B900) and the post-effect frustum setup (0x48C9D0): tan(fov/2) is
     scaled by max(1, aspect * 9/16), so the vertical FOV stays as at 16:9 and the view gets
     wider (Hor+) instead of cropped. The culling frustum (0x44B9E0) gets the matching angle
     2*atan(k*tan(fov/2)), otherwise objects vanish near the screen edges.
  4. The two places that set gOrthoScaling for 2D/HUD (0x463EB0, 0x4641F0): the virtual
     1920x1080 screen is fitted into a centred 16:9 area so the HUD and menus are not stretched.
  5. The point sprite vertex shader (particles, sparkles, glows): sprites are as wide as their
     texture instead of (screen width / height) times wider. This one also fixes plain 16:9.
The code caves live in the zero padding at the end of .text (0x6D5AE0..0x6D5C00); the .text
VirtualSize is extended to cover them (still inside the same page, image size unchanged).
"""
import hashlib
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(os.path.dirname(HERE), "Vince.exe")
BACKUP = EXE + ".before-ultrawide"

IMAGE_BASE = 0x400000
TEXT_VA = 0x401000
TEXT_RAW = 0x400
TEXT_VSIZE_OLD = 0x2D4AD8
TEXT_VSIZE_NEW = 0x2D4C00          # = SizeOfRawData, still below the next section (0x2D5000)
CAVE = 0x6D5AE0
CAVE_END = 0x6D5C00

# game functions the caves call / jump to
FN_TANF = 0x4222D0                  # float tanf(xmm0) wrapper
FN_TAN = 0x68A8D0                   # double tan(xmm0) CRT thunk
FN_SET_CONST = 0x45F110             # Renderer::SetShaderConstant(stage, slot, float4*, count)


def va2off(va):
    if TEXT_VA <= va < TEXT_VA + TEXT_VSIZE_NEW:
        return TEXT_RAW + (va - TEXT_VA)
    raise ValueError("address outside .text: %#x" % va)


def rel32(src_next, dst):
    return struct.pack("<i", dst - src_next)


class Asm:
    """Tiny helper: raw bytes plus labels / rel32 fixups."""

    def __init__(self, base):
        self.base = base
        self.buf = bytearray()
        self.labels = {}
        self.fixups = []            # (offset of rel32, label or absolute va)

    @property
    def here(self):
        return self.base + len(self.buf)

    def label(self, name):
        self.labels[name] = self.here

    def b(self, hexstr):
        self.buf += bytes.fromhex(hexstr.replace(" ", ""))

    def imm32(self, value):
        self.buf += struct.pack("<I", value)

    def call(self, target):
        self.buf += b"\xE8"
        self.fixups.append((len(self.buf), target))
        self.buf += b"\0\0\0\0"

    def jmp(self, target):
        self.buf += b"\xE9"
        self.fixups.append((len(self.buf), target))
        self.buf += b"\0\0\0\0"

    def jcc8(self, opcode, name):
        self.buf += bytes([opcode, 0])
        self.fixups.append((len(self.buf) - 1, ("short", name)))

    def finish(self):
        for off, target in self.fixups:
            if isinstance(target, tuple):
                dst = self.labels[target[1]]
                d = dst - (self.base + off + 1)
                assert -128 <= d < 128
                self.buf[off] = d & 0xFF
            else:
                dst = self.labels[target] if isinstance(target, str) else target
                self.buf[off:off + 4] = rel32(self.base + off + 4, dst)
        return bytes(self.buf)


F_ONE = 0x3F800000      # 1.0
F_9_16 = 0x3F100000     # 0.5625
F_960 = 0x44700000      # 960.0  (half of the virtual 1920)
F_540 = 0x44070000      # 540.0  (half of the virtual 1080)
F_HALF = 0x3F000000     # 0.5
CAVE2 = 0x6D5BC8        # second cave block (frustum), added in v1.1


def build_caves(cave=CAVE, cave2=CAVE2, cave_end=CAVE_END):
    a = Asm(cave)

    # hor_k: xmm1 = aspect  ->  xmm1 = max(1, aspect * 9/16). Touches nothing else.
    a.label("hor_k")
    a.b("68"); a.imm32(F_9_16)              # push 0.5625
    a.b("f3 0f 59 0c 24")                   # mulss xmm1, [esp]
    a.b("c7 04 24"); a.imm32(F_ONE)         # mov dword [esp], 1.0
    a.b("f3 0f 5f 0c 24")                   # maxss xmm1, [esp]   (NaN -> 1.0)
    a.b("8d 64 24 04")                      # lea esp, [esp+4]
    a.b("c3")                               # ret

    # ctor_aspect: eax = display. Out: xmm2 = aspect, xmm0 = float(display+0x20) like the original.
    # Must not touch xmm1 (argument of the following call) or any GPR.
    a.label("ctor_aspect")
    a.b("f3 0f 10 50 44")                   # movss xmm2, [eax+0x44]   viewport width
    a.b("f3 0f 10 40 48")                   # movss xmm0, [eax+0x48]   viewport height
    a.b("68"); a.imm32(F_ONE)               # push 1.0
    a.b("0f 2f 04 24")                      # comiss xmm0, [esp]
    a.b("8d 64 24 04")                      # lea esp, [esp+4]   (keeps flags)
    a.jcc8(0x72, "ctor_fallback")           # jb  (height < 1 or NaN: display not set up yet)
    a.b("f3 0f 5e d0")                      # divss xmm2, xmm0
    a.jcc8(0xEB, "ctor_done")               # jmp short
    a.label("ctor_fallback")
    a.b("66 0f 6e 50 1c")                   # movd xmm2, [eax+0x1c]   (original: 1920/1080)
    a.b("0f 5b d2")                         # cvtdq2ps xmm2, xmm2
    a.b("66 0f 6e 40 20")                   # movd xmm0, [eax+0x20]
    a.b("0f 5b c0")                         # cvtdq2ps xmm0, xmm0
    a.b("f3 0f 5e d0")                      # divss xmm2, xmm0
    a.label("ctor_done")
    a.b("66 0f 6e 40 20")                   # movd xmm0, [eax+0x20]
    a.b("0f 5b c0")                         # cvtdq2ps xmm0, xmm0
    a.b("c3")                               # ret

    # proj_tan: replaces "call tanf" in Camera::UpdateProjection (edi = camera).
    a.label("proj_tan")
    a.call(FN_TANF)                         # xmm0 = tan(fov/2)
    a.b("f3 0f 10 8f b0 00 00 00")          # movss xmm1, [edi+0xb0]   camera aspect
    a.call("hor_k")
    a.b("f3 0f 59 c1")                      # mulss xmm0, xmm1
    a.b("c3")                               # ret

    # fx_tan: replaces "call tan" (double) in the post-effect frustum setup (esi = camera).
    a.label("fx_tan")
    a.call(FN_TAN)                          # xmm0 = tan(fov/2) as double
    a.b("f2 0f 5a c0")                      # cvtsd2ss xmm0, xmm0
    a.b("f3 0f 10 8e b0 00 00 00")          # movss xmm1, [esi+0xb0]
    a.call("hor_k")
    a.b("f3 0f 59 c1")                      # mulss xmm0, xmm1
    a.b("f3 0f 5a c0")                      # cvtss2sd xmm0, xmm0
    a.b("c3")                               # ret

    # ortho_fit: replaces "call SetShaderConstant(0, 0x2F /*gOrthoScaling*/, ptr, 1)".
    # ptr -> {offset x, offset y, scale x, scale y}; scale 1.0 = 1920x1080 pixels.
    # Uses the smaller scale for both axes and centres the 1920x1080 area, then tail-jumps.
    a.label("ortho_fit")
    a.b("8b 44 24 0c")                      # mov eax, [esp+0xc]   (the float4*)
    a.b("f3 0f 10 40 08")                   # movss xmm0, [eax+8]
    a.b("f3 0f 10 48 0c")                   # movss xmm1, [eax+0xc]
    a.b("0f 28 d0")                         # movaps xmm2, xmm0
    a.b("f3 0f 5d d1")                      # minss xmm2, xmm1      s = min(sx, sy)
    a.b("f3 0f 5c c2")                      # subss xmm0, xmm2
    a.b("f3 0f 5c ca")                      # subss xmm1, xmm2
    a.b("68"); a.imm32(F_960)               # push 960.0
    a.b("f3 0f 59 04 24")                   # mulss xmm0, [esp]
    a.b("c7 04 24"); a.imm32(F_540)         # mov dword [esp], 540.0
    a.b("f3 0f 59 0c 24")                   # mulss xmm1, [esp]
    a.b("8d 64 24 04")                      # lea esp, [esp+4]
    a.b("f3 0f 58 00")                      # addss xmm0, [eax]
    a.b("f3 0f 58 48 04")                   # addss xmm1, [eax+4]
    a.b("f3 0f 11 00")                      # movss [eax], xmm0
    a.b("f3 0f 11 48 04")                   # movss [eax+4], xmm1
    a.b("f3 0f 11 50 08")                   # movss [eax+8], xmm2
    a.b("f3 0f 11 50 0c")                   # movss [eax+0xc], xmm2
    a.jmp(FN_SET_CONST)

    code = a.finish()
    assert cave + len(code) <= cave2, "cave too large"

    # frustum_fov: replaces "mov eax,[esi]; mov eax,[eax+0x38]; call eax" in Camera::UpdateFrustum
    # (esi = ecx = camera). Returns st0 = 2*atan(k*tan(fov/2)), the Hor+ horizontal FOV; the
    # frustum code divides the vertical part by the aspect, which then matches the 16:9 view.
    b = Asm(cave2)
    b.label("frustum_fov")
    b.b("8b 06")                            # mov eax, [esi]
    b.b("ff 50 38")                         # call [eax+0x38]   st0 = fov
    b.b("f3 0f 10 8e b0 00 00 00")          # movss xmm1, [esi+0xb0]
    b.call(a.labels["hor_k"])               # xmm1 = k
    b.b("68"); b.imm32(F_HALF)              # push 0.5
    b.b("d8 0c 24")                         # fmul dword [esp]     fov/2
    b.b("d9 f2")                            # fptan                st0 = 1, st1 = tan
    b.b("dd d8")                            # fstp st0
    b.b("f3 0f 11 0c 24")                   # movss [esp], xmm1
    b.b("d8 0c 24")                         # fmul dword [esp]     k*tan
    b.b("d9 e8")                            # fld1
    b.b("d9 f3")                            # fpatan               atan(k*tan)
    b.b("d8 c0")                            # fadd st0, st0
    b.b("8d 64 24 04")                      # lea esp, [esp+4]
    b.b("c3")                               # ret
    code2 = b.finish()
    assert cave2 + len(code2) <= cave_end, "cave 2 too large"
    labels = dict(a.labels)
    labels.update(b.labels)
    return code, code2, labels


def call_bytes(at, target):
    return b"\xE8" + rel32(at + 5, target)


NOP7 = bytes.fromhex("0f1f8000000000")


def build_patches(cave=CAVE, cave2=CAVE2, cave_end=CAVE_END):
    code, code2, L = build_caves(cave, cave2, cave_end)
    P = []  # (name, va, original, patched)
    P.append(("viewport: no pillarbox when wider than 16:9", 0x46737D,
              bytes.fromhex("f30f59f1"),            # mulss xmm6, xmm1   (width = height * 16/9)
              bytes.fromhex("0f28f490")))           # movaps xmm6, xmm4  (width = screen width); nop
    P.append(("camera aspect from the real viewport", 0x44B424,
              bytes.fromhex("660f6e501c660f6e40200f5bd20f5bc0f30f5ed0"),
              call_bytes(0x44B424, L["ctor_aspect"]) + NOP7 + NOP7 + b"\x90"))
    P.append(("Hor+ FOV: camera projection", 0x44B933,
              call_bytes(0x44B933, FN_TANF), call_bytes(0x44B933, L["proj_tan"])))
    P.append(("Hor+ FOV: post-effect frustum", 0x48CDBE,
              call_bytes(0x48CDBE, FN_TAN), call_bytes(0x48CDBE, L["fx_tan"])))
    P.append(("HUD centred 16:9 (scene pass)", 0x46415E,
              call_bytes(0x46415E, FN_SET_CONST), call_bytes(0x46415E, L["ortho_fit"])))
    P.append(("HUD centred 16:9 (screen pass)", 0x464561,
              call_bytes(0x464561, FN_SET_CONST), call_bytes(0x464561, L["ortho_fit"])))
    P.append(("code caves", cave, bytes(len(code)), code))
    P.append(("Hor+ FOV: culling frustum", 0x44BA16,
              bytes.fromhex("8b068b4038ffd0"),      # mov eax,[esi]; mov eax,[eax+0x38]; call eax
              call_bytes(0x44BA16, L["frustum_fov"]) + b"\x66\x90"))
    P.append(("code caves (frustum)", cave2, bytes(len(code2)), code2))
    return P


def text_header_offset(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        if data[o:o + 8].rstrip(b"\0") == b".text":
            return o
    raise RuntimeError(".text section not found")


# PointSpriteVS (particles, sparkles, glows...): the game offsets the sprite corners in clip space by
# the same amount in x and y, so every sprite is (screen width / height) times wider than its
# texture (also at 16:9). The replacement is the same shader with scale.x multiplied by
# |viewproj column 0| / |viewproj column 1| = height / width (source: PointSpriteFixed.hlsl),
# compiled with d3dcompiler_47 for vs_4_1, identical input/output signatures, and padded with a
# private-data chunk to the original size so the game's shader table (ptr + size) stays valid.
SPRITE_OFF = 0x638790               # file offset of the DXBC blob (VA 0xA39790, in .rdata)
SPRITE_SIZE = 0xCA10
SPRITE_ORIG_SHA = "31f61468c8c3a2e6ee6ab1ebccb47d56b6df3fc93131f66bf72b89fbc0580bec"
SPRITE_NEW_SHA = "5b98b2fd58422473c04a28732329e72c110b6783eed8ddd7c7d118c0811d2eb7"
SPRITE_ORIG_FILE = os.path.join(HERE, "pointsprite_vs_original.bin")
SPRITE_NEW_FILE = os.path.join(HERE, "pointsprite_vs_fixed.bin")  # the replacement blob (was the SPRITE_NEW_Z literal)


def sha(b):
    return hashlib.sha256(b).hexdigest()


def sprite_new():
    """The replacement PointSpriteVS blob, read from the .bin beside this script."""
    with open(SPRITE_NEW_FILE, "rb") as f:
        return f.read()


def sprite_patch(data):
    """(name, offset, original, patched). The original blob comes from the exe itself, the saved
    copy next to this script, or the backup; None if none of them has the expected bytes."""
    new = sprite_new()
    assert len(new) == SPRITE_SIZE and sha(new) == SPRITE_NEW_SHA
    orig = None
    candidates = [bytes(data[SPRITE_OFF:SPRITE_OFF + SPRITE_SIZE])]
    if os.path.exists(SPRITE_ORIG_FILE):
        candidates.append(open(SPRITE_ORIG_FILE, "rb").read())
    if os.path.exists(BACKUP):
        with open(BACKUP, "rb") as f:
            f.seek(SPRITE_OFF)
            candidates.append(f.read(SPRITE_SIZE))
    for c in candidates:
        if sha(c) == SPRITE_ORIG_SHA:
            orig = c
            break
    return ("point sprites: same shape as their texture", SPRITE_OFF, orig or bytes(SPRITE_SIZE), new)


def all_patches(data):
    """Code patches, the .text VirtualSize header field and the point sprite shader."""
    P = [(n, va2off(va), o, p) for n, va, o, p in build_patches()]
    vs = text_header_offset(data) + 8
    P.append((".text VirtualSize covers the caves", vs,
              struct.pack("<I", TEXT_VSIZE_OLD), struct.pack("<I", TEXT_VSIZE_NEW)))
    P.append(sprite_patch(data))
    return P


def state(data, off, orig, new):
    cur = data[off:off + len(orig)]
    if cur == new:
        return "applied"
    if cur == orig:
        return "original"
    return "unknown"


def main():
    cmd = (sys.argv[1] if len(sys.argv) > 1 else "apply").lower()
    if not os.path.isfile(EXE):
        sys.exit("Vince.exe not found next to this folder: %s" % EXE)
    data = bytearray(open(EXE, "rb").read())
    P = all_patches(data)
    states = [state(data, off, o, p) for _, off, o, p in P]

    if cmd == "status":
        for (n, off, _, _), s in zip(P, states):
            print("  %-45s %s" % (n, s))
        return

    if cmd == "dryrun":
        try:
            from capstone import Cs, CS_ARCH_X86, CS_MODE_32
            md = Cs(CS_ARCH_X86, CS_MODE_32)
        except ImportError:
            md = None
        for (n, off, o, p), s in zip(P, states):
            va = TEXT_VA + off - TEXT_RAW
            print("== %s  (file %#x, %s)" % (n, off, s))
            if off == SPRITE_OFF:
                print("   DXBC blob %d bytes: sha256 %s -> %s" % (len(p), sha(o)[:16], sha(p)[:16]))
            elif md and n != ".text VirtualSize covers the caves":
                for tag, blob in (("old", o), ("new", p)):
                    if tag == "old" and n == "code caves":
                        continue
                    for ins in md.disasm(blob, va):
                        print("   %s %08X  %-22s %s %s" % (tag, ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
            else:
                print("   %s -> %s" % (o.hex(), p.hex()))
        return

    if cmd in ("apply", "install"):
        bad = [n for (n, *_), s in zip(P, states) if s == "unknown"]
        if bad:
            sys.exit("Unexpected bytes in Vince.exe (different game version or another patch?):\n  "
                     + "\n  ".join(bad) + "\nNothing was changed.")
        if all(s == "applied" for s in states):
            print("Already applied.")
            return
        if not os.path.exists(BACKUP):
            shutil.copy2(EXE, BACKUP)
            print("Backup: %s" % BACKUP)
        sprite_orig = P[-1][2]
        if not os.path.exists(SPRITE_ORIG_FILE) and sha(sprite_orig) == SPRITE_ORIG_SHA:
            open(SPRITE_ORIG_FILE, "wb").write(sprite_orig)
        for _, off, o, p in P:
            data[off:off + len(p)] = p
        open(EXE, "wb").write(data)
        print("Ultrawide patch applied.")
        return

    if cmd in ("uninstall", "remove"):
        bad = [n for (n, *_), s in zip(P, states) if s == "unknown"]
        if bad:
            sys.exit("Unexpected bytes, not touching the file:\n  " + "\n  ".join(bad)
                     + "\nRestore %s instead." % os.path.basename(BACKUP))
        for _, off, o, p in P:
            data[off:off + len(o)] = o
        open(EXE, "wb").write(data)
        print("Ultrawide patch removed (original bytes restored).")
        return

    sys.exit(__doc__)


if __name__ == "__main__":
    main()
