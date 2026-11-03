"""AIO build (phase 1.4): compose the always-on exe components onto stock Vince.exe in memory.

Usage: python -I aio/build.py exe --stock <oracle dir> [--out <dir>]   (default out: build/aio)

Reads <oracle>/Vince.exe (never written), runs each step of aio/exe_chain.json through its adapter on the
bytes in memory, writes <out>/Vince.exe, <out>/stages/NN-<component>[-<sets>].exe (each step's output as the
applier returned it) and <out>/ownership.json. Every adapter loads the component's own applier from its folder
(importlib, no sys.path change) and compares the bytes it patches before writing; no installer or CLI runs and
nothing is written into a component folder. When a step refuses for lack of a section-header slot (measured:
free slots before the step < sections the step adds), a build-owned header_move step is inserted before it
(NT headers + section table to 0x40, as Cutscenewalk/tools/patch_walkout.py:141-176). The PE CheckSum is set
once, after the last step. ownership.json: byte diffs of each stage against its input; header bookkeeping
(NumberOfSections, SizeOfCode, SizeOfImage, CheckSum, data directories, .reloc) and header_move go to "build".
After the CheckSum, check_layout re-reads the final image (read only) and refuses unless: one header_move, e_lfanew
0x40, the 7 stock section headers byte-identical, the added sections exactly ADDED (order, flags), page-contiguous
from 0x87A000 with SizeOfImage at the end of the last, imports (dirs 1, 12, descriptor bytes, parsed thunks) equal
stock's, and .vbeam/.vfogz hold their applier's payload_at for the placed VA (.vfogd at .vfogz + rva_offset). main
prints the layout (header slots, one line per section, link_va -> placed) and ownership.json keeps it under "layout".

Exit codes: 0 ok, 1 a step refused (its text is printed), 2 bad input.

       python -I aio/build.py dll [--out <dir>]   (default out: build/aio; 1.4 T6)

Builds the AIO core <out>/X3DAudio1_7.dll and prints its sha256: stops (exit 2) unless clang and lld-link in
toolchain.LLVM_BIN are LLVM_VERSION at LLVM_COMMIT; compiles VinceFix, VinceAudio and VinceSteam (each with its own
builder's flags) and aio/core/core.c, all with -DVINCE_AIO, plus VinceFix's thunks.s in a scratch dir under
.run/<phase>/ (deleted after); links /Brepro with aio/core/exports.def and the aio/core/*.def import libraries; then
refuses (exit 1) unless the DLL is PE32 x86 with exactly the two X3DAudio exports (no forwarders), imports exactly
aio/core/imports-allowed.txt, has DYNAMIC_BASE, NX_COMPAT and NO_SEH (or an SEHandlerTable), and holds no repo path.
Also copies the settings template aio/core/VinceAIO.ini to <out>/VinceAIO.ini (byte copy; 1.4 T7).

       python -I aio/build.py data --stock <oracle dir> [--out <dir>]   (default out: build/aio; 1.5 T1)

Runs aio/data_chain.json in memory: each step reads its input (game-relative under the oracle, or repo-relative
with from=repo; input sha256 pinned), calls the component's pure entry (loaded by path, never its installer) or
copies, and asserts its pin: whole-file sha256, and for HOT archives the changed decompressed entries (entry table
unchanged) with their sha256 plus any pinned report keys (bog matches_reference). A whole-file mismatch where entry
pins hold is reported as entry-level, naming both zlibs (I3). Only after every step passes: writes each output at
its game-relative path under <out> (release-held components under <out>/held/<component>/), the water d3d11.dll
(VinceWaterFix/tools/build_native.py build(), sha256 pinned in data_chain.json "water"; 1.5 T4) with
VinceWaterFix.NOTICES.txt, and <out>/ownership-data.json (zlib.ZLIB_RUNTIME_VERSION, per output: input, shas, changed entries, held, pin result).

       python -I aio/build.py options --stock <oracle dir> [--out <dir>]   (default out: build/aio; 1.6 T5)

Runs aio/options_chain.json in memory: composes the exe as `exe` (CheckSum, check_layout), asserts the base sha256,
builds the exe variants (none, voodoopowerfix, firebug, both) with their apply/final and aio/components.json range
pins, then the option data steps (run_step, the same step body as `data`; input from stock, the "over" set's output,
or the always-on data_chain output on that path). Only after every pin holds: replaces <out>/options/, writes
options/<set>/Vince.exe and released data at options/<set>/<game path> (set = none or ids joined by +, order
voodoopowerfix, firebug, retexture), held data at held/<component>/<game path>, and <out>/ownership-options.json
(files, and the 8 combinations: per game path the file built for exactly its enabled options that touch it).
"""
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

import pefile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CHAIN = HERE / "exe_chain.json"
SCHEMA = 1


def _phase():
    # "# Phase 1.4 — ..." -> "1.4"; scratch lives under .run/<phase>/ only
    # env VV_PHASE names it between phases, when there is no PHASE_PLAN.md (2026-10-30)
    if os.environ.get("VV_PHASE"):
        return os.environ["VV_PHASE"]
    plan = ROOT / "phase-ends" / "current" / "PHASE_PLAN.md"
    if not plan.exists():  # the public source tree (release.py public) has no plan: scratch under .run/build/
        return "build"
    head = plan.read_text(encoding="utf-8").split("\n", 1)[0].split()
    assert len(head) >= 3 and head[1] == "Phase", "PHASE_PLAN.md header"
    return head[2]


SCRATCH = ROOT / ".run" / _phase()


class Refused(Exception):
    pass


_modules = {}


def module(rel):
    """The component file at ROOT/rel, loaded once without touching sys.path."""
    if rel not in _modules:
        path = ROOT / rel
        spec = importlib.util.spec_from_file_location("aio_" + "".join(c if c.isalnum() else "_" for c in rel), path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _modules[rel] = mod
    return _modules[rel]


def load_json(rel):
    return json.loads((ROOT / rel).read_text(encoding="utf-8"))


# ---- adapters: (data: bytes, step: dict) -> bytes; Refused on any refusal ----

def adapt_lightning(data, step):
    try:
        return module("VinceLightningfix/tools/patch_lightning.py").build_patch(data, identity=False)[0]
    except ValueError as e:
        raise Refused(str(e)) from None


def adapt_vinceaudio(data, step):
    ps = module("Xaudio2 rework/scripts/patchset.py")
    text = ps.build_spec()
    spec = json.loads(text)
    sets = step["sets"]
    if "loader" in sets or not set(sets) <= set(spec["composition"]):
        raise Refused(f"sets {sets} not allowed (loader is never applied)")
    spec["composition"] = [p for p in spec["composition"] if p in sets]
    work = SCRATCH / "build_work"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    try:
        out, footprint = ps.apply_spec(data, spec, hashlib.sha256(text).hexdigest(), work=work)
    except ValueError as e:
        raise Refused(str(e)) from None
    finally:
        shutil.rmtree(work, ignore_errors=True)
    applied = {p for r in footprint["regions"] for p in r["patch"].split("+")}
    if not set(sets) <= applied:
        raise Refused(f"sets {sorted(set(sets) - applied)} already present; nothing applied for them")
    return out


def _identity_off(rel, name, data, **kw):
    """rel's build_patch(data, identity=False) output; the payload work dir (if any) is deleted after."""
    work = SCRATCH / "build_work" / name
    shutil.rmtree(work, ignore_errors=True)
    try:
        if kw.pop("compiles", True):
            kw["work"] = work
        return module(rel).build_patch(data, identity=False, **kw)[0]
    except ValueError as e:
        raise Refused(str(e)) from None
    finally:
        shutil.rmtree(work, ignore_errors=True)


def adapt_charmainefix_narration(data, step):
    return _identity_off("Charmainefix/tools/patch_narration.py", "charmainefix_narration", data)


def adapt_charmainefix_timing(data, step):
    return _identity_off("Charmainefix/tools/patch_timing.py", "charmainefix_timing", data,
                         compiles=False, version=3)


def adapt_cutscenewalk(data, step):
    return _identity_off("Cutscenewalk/tools/patch_walkout.py", "cutscenewalk", data)


def adapt_cutscenefix(data, step):
    return _identity_off("VinceParticuleoverload/tools/patch_cutscenefix.py", "cutscenefix", data)


def adapt_smokefix(data, step):
    mod = module("Vincesmokefix/install_patch.py")
    ps = load_json("Vincesmokefix/patchset.json")
    try:
        states = mod.status(data, ps, identity=False)
        if set(states.values()) != {"stock"}:
            raise Refused(f"sites not stock: {states}")
        return mod.apply(data, ps, identity=False)
    except (ValueError, mod.Refused) as e:
        raise Refused(str(e)) from None


def _classify_apply(rel, spec_rel, data):
    mod = module(rel)
    spec = load_json(spec_rel)
    try:
        state, _, states = mod.classify(spec, data)
        if state != "base":
            raise Refused(f"{state}: sites {states}")
        out = mod.apply(spec, data)
        if mod.classify(spec, out)[0] != "installed":
            raise Refused("not installed after apply")
        return out
    except ValueError as e:
        raise Refused(str(e)) from None


def adapt_lighthouse(data, step):
    return _classify_apply("VinceLighthouseFix/install_patch.py", "VinceLighthouseFix/patchset.json", data)


def adapt_fogsort(data, step):
    return _classify_apply("VinceFogSortFix/install_patch.py", "VinceFogSortFix/patchset.json", data)


def _put(data, off, before, after, what):
    """data with after at off; Refused unless the bytes there are before."""
    if data[off:off + len(before)] != before:
        raise Refused(f"{what}: unexpected bytes at file {off:#x}")
    return data[:off] + after + data[off + len(after):]


def adapt_lightglow(data, step):
    mod = module("VinceLightGlowFix/install_patch.py")
    try:
        state, off, current = mod.classify(data)       # table found via the section headers
        if state != "base":
            raise Refused(f"{state}: node-type table {current.hex()}")
        return _put(data, off, mod.ORIGINAL, mod.PATCHED, "lightglow table")
    except (SystemExit, ValueError, AssertionError) as e:
        raise Refused(str(e)) from None


def adapt_spooky(data, step):
    mod = module("VinceSpookyFix/fix/patch_spooky_fov.py")
    try:
        off, size = mod.SHADER_OFFSET, mod.SHADER_SIZE
        current = data[off:off + size]
        if current[:4] != b"DXBC" or mod.hash_str(current) != mod.ORIGINAL_HASH \
                or mod.dxbc_checksum(current) != current[4:20]:
            raise Refused(f"SpookyVS at file {off:#x} is not the original shader")
        patched = mod.pad_to(mod.FIXED_SHADER, size)
        assert len(patched) == size and mod.dxbc_checksum(patched) == patched[4:20]
        return _put(data, off, current, patched, "SpookyVS")
    except (SystemExit, ValueError, AssertionError) as e:
        raise Refused(str(e)) from None


AIO_NEAR = 0.05  # was nearplane_patch.DEFAULT_NEAR 0.1; developer's in-game pick 2026-10-30 (1.7 T13, docs/aio/INGAME_CHECKLIST.md:79-82)


def adapt_nearplane(data, step):
    mod = module("nearplane_patch.py")
    try:
        off, _ = mod.find_near(data)                   # exactly one pattern hit in .text
        return _put(data, off, struct.pack("<f", mod.ORIGINAL_NEAR), struct.pack("<f", AIO_NEAR),
                    "near plane")
    except (SystemExit, ValueError, AssertionError) as e:
        raise Refused(str(e)) from None


UW_SPRITE_VA = 0xA39790   # PointSpriteVS DXBC, ultrawide/ultrawide_patch.py:275 (file SPRITE_OFF on stock)
UW_SECTION = b".vwide"


def _align(n, a):
    return (n + a - 1) // a * a


def _v2f(p, va, n):
    """File offset of VA va..va+n, which must lie inside one section's VirtualSize and raw data."""
    rva = va - 0x400000
    for _, vs, sva, rs, rp in p["secs"]:
        if sva <= rva and rva + n <= sva + min(vs, rs):
            return rp + rva - sva
    raise Refused(f"VA {va:#x}+{n} is not inside a section")


def adapt_ultrawide(data, step):
    """ultrawide on its own appended section .vwide: the CLI's caves (in .text padding, .text VirtualSize
    extended) move to .vwide at the same internal layout; .text VirtualSize is never touched."""
    uw = module("ultrawide/ultrawide_patch.py")
    try:
        p = pe_info(data)
        opt = p["opt"]
        base, salign, falign = struct.unpack_from("<III", data, opt + 28)
        if base != uw.IMAGE_BASE:
            raise Refused(f"ImageBase {base:#x}")
        if any(name == UW_SECTION for name, *_ in p["secs"]):
            raise Refused(".vwide already present")
        if p["ndirs"] > 4 and struct.unpack_from("<I", data, opt + 96 + 8 * 4)[0]:
            raise Refused("exe has a certificate table")
        if len(data) != max(rp + rs for _, _, _, rs, rp in p["secs"]):
            raise Refused("data after the last section (overlay) is unsupported")
        if free_slots(data) < 1:
            raise Refused("No free section-header slot")
        size = uw.CAVE_END - uw.CAVE                   # 0x120
        last = max(p["secs"], key=lambda s: s[2])
        va = _align(last[2] + max(last[1], last[3]), salign)
        raw, rsize = _align(len(data), falign), _align(size, falign)
        b = bytearray(data) + bytes(raw - len(data) + rsize)
        hdr = p["table"] + 40 * p["count"]
        b[hdr:hdr + 40] = UW_SECTION.ljust(8, b"\0") + struct.pack("<IIIIIIHHI", size, va, rsize, raw,
                                                                   0, 0, 0, 0, 0x60000020)
        struct.pack_into("<H", b, p["lfanew"] + 6, p["count"] + 1)
        struct.pack_into("<I", b, opt + 56, _align(va + size, salign))
        cave = uw.IMAGE_BASE + va
        sites = [(name, at, before, after) for name, at, before, after
                 in uw.build_patches(cave, cave + (uw.CAVE2 - uw.CAVE), cave + size)]
        new = uw.sprite_new()   # as sprite_patch(): pointsprite_vs_fixed.bin (was the SPRITE_NEW_Z literal)
        assert len(new) == uw.SPRITE_SIZE and uw.sha(new) == uw.SPRITE_NEW_SHA
        q = pe_info(bytes(b))
        if _v2f(q, UW_SPRITE_VA, uw.SPRITE_SIZE) != uw.SPRITE_OFF:
            raise Refused(f"PointSpriteVS VA {UW_SPRITE_VA:#x} is not at file {uw.SPRITE_OFF:#x}")
        current = bytes(b[uw.SPRITE_OFF:uw.SPRITE_OFF + uw.SPRITE_SIZE])
        if uw.sha(current) != uw.SPRITE_ORIG_SHA:
            raise Refused("point sprites: PointSpriteVS is not the original shader")
        sites.append(("point sprites", UW_SPRITE_VA, current, new))
        out = bytes(b)
        for name, at, before, after in sites:
            out = _put(out, _v2f(q, at, len(after)), before, after, name)
        return out
    except (SystemExit, ValueError, AssertionError) as e:
        raise Refused(str(e)) from None


# ---- PE helpers (struct only) ----

def pe_info(data):
    lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[lfanew:lfanew + 4] != b"PE\0\0":
        raise Refused("not a PE file")
    count, optsize = struct.unpack_from("<HxxxxxxxxxxxxH", data, lfanew + 6)
    opt = lfanew + 24
    table = opt + optsize
    secs = []
    for i in range(count):
        at = table + 40 * i
        name = data[at:at + 8].rstrip(b"\0")
        vs, va, rs, rp = struct.unpack_from("<IIII", data, at + 8)
        secs.append((name, vs, va, rs, rp))
    return {"lfanew": lfanew, "count": count, "optsize": optsize, "opt": opt, "table": table, "secs": secs,
            "size_of_headers": struct.unpack_from("<I", data, opt + 60)[0],
            "ndirs": struct.unpack_from("<I", data, opt + 92)[0]}


def free_slots(data):
    """Section-header slots free after the table: zero 40-byte blocks before SizeOfHeaders and the first raw data."""
    p = pe_info(data)
    limit = min([p["size_of_headers"]] + [rp for _, _, _, rs, rp in p["secs"] if rs])
    at, n = p["table"] + 40 * p["count"], 0
    while at + 40 <= limit and not any(data[at:at + 40]):
        at, n = at + 40, n + 1
    return n


def header_move(data, step=None):
    """NT headers + section table to right after the DOS header (0x40), as patch_walkout.py:141-176."""
    p = pe_info(data)
    for i in range(min(p["ndirs"], 16)):
        rva = struct.unpack_from("<I", data, p["opt"] + 96 + 8 * i)[0]
        if rva and rva < p["size_of_headers"]:
            raise Refused(f"data directory {i} lies in the headers")
    if len(data) != max(rp + rs for _, _, _, rs, rp in p["secs"]):
        raise Refused("data after the last section (overlay) is unsupported")
    if p["lfanew"] <= 0x40:
        raise Refused("headers already at 0x40")
    headers = 4 + 20 + p["optsize"] + 40 * p["count"]
    b = bytearray(data)
    block = bytes(b[p["lfanew"]:p["lfanew"] + headers])
    b[0x40:p["size_of_headers"]] = bytes(p["size_of_headers"] - 0x40)
    b[0x40:0x40 + headers] = block
    struct.pack_into("<I", b, 0x3C, 0x40)
    return bytes(b)


ADAPTERS = {"lightning": adapt_lightning, "vinceaudio": adapt_vinceaudio, "smokefix": adapt_smokefix,
            "lighthouse": adapt_lighthouse, "fogsort": adapt_fogsort, "lightglow": adapt_lightglow,
            "spooky": adapt_spooky, "nearplane": adapt_nearplane, "ultrawide": adapt_ultrawide,
            "charmainefix_narration": adapt_charmainefix_narration,
            "charmainefix_timing": adapt_charmainefix_timing, "cutscenewalk": adapt_cutscenewalk,
            "cutscenefix": adapt_cutscenefix, "header_move": header_move}


# ---- ownership ----

def changed(before, after):
    """Sorted file offsets where after differs from before; bytes past len(before) all count."""
    out, n, chunk = [], min(len(before), len(after)), 4096
    for c in range(0, n, chunk):
        if before[c:c + chunk] != after[c:c + chunk]:
            e = min(c + chunk, n)
            out.extend(i for i in range(c, e) if before[i] != after[i])
    out.extend(range(n, len(after)))
    return out


def build_offsets(data):
    """File offsets of the header fields and .reloc bytes the build owns, in image data."""
    p = pe_info(data)
    opt, own = p["opt"], set()
    for at, n in [(p["lfanew"] + 6, 2), (opt + 4, 4), (opt + 56, 4), (opt + 64, 4),
                  (opt + 96, 8 * min(p["ndirs"], 16))]:
        own.update(range(at, at + n))
    for name, _, _, rs, rp in p["secs"]:
        if name == b".reloc":
            own.update(range(rp, rp + rs))
    return own


def ranges(offsets):
    out = []
    for i in offsets:
        if out and out[-1][1] == i:
            out[-1][1] = i + 1
        else:
            out.append([i, i + 1])
    return out


def stage_name(index, step):
    sets = "".join(step.get("sets") or [])
    return f"{index:02d}-{step['component']}" + (f"-{sets}" if sets else "") + ".exe"


def compose(stock):
    """[(step, stage name, input bytes, output bytes)] for the chain on stock; Refused names the step."""
    steps = json.loads(CHAIN.read_text(encoding="utf-8"))
    if steps.get("schema") != SCHEMA:
        raise Refused(f"{CHAIN.name}: schema {steps.get('schema')}")
    done, data = [], stock

    def record(step, out):
        done.append((step, stage_name(len(done) + 1, step), data, out))
        return out

    for step in steps["steps"]:
        run = ADAPTERS[step["adapter"]]
        try:
            out = run(data, step)
        except Refused as first:
            # measure: does the step need more header slots than this image has?
            try:
                moved = header_move(data)
                trial = run(moved, step)
            except Refused:
                raise Refused(f"{step['component']} {step.get('sets') or ''}: {first}") from None
            need = pe_info(trial)["count"] - pe_info(data)["count"]
            if free_slots(data) >= need:
                raise Refused(f"{step['component']} {step.get('sets') or ''}: {first}") from None
            data = record({"component": "build", "adapter": "header_move",
                           "why": f"{step['component']} adds {need} section(s), {free_slots(data)} slot(s) free"},
                          moved)
            out = trial
        data = record(step, out)
    return done


def set_checksum(data):
    b = bytearray(data)
    pe = pefile.PE(data=bytes(b), fast_load=True)
    struct.pack_into("<I", b, pe.OPTIONAL_HEADER.get_field_absolute_offset("CheckSum"), pe.generate_checksum())
    pe.close()
    return bytes(b)


# ---- layout check (reads only) ----

IMAGE_BASE = 0x400000
FIRST_ADDED_VA = 0x87A000
ADDED = [  # (name, characteristics) in chain order
    (b".vlight", 0x60000020),   # VinceLightningfix/tools/patch_lightning.py:128
    (b".vacfix", 0x60000020),   # Xaudio2 rework/build/patchset/patchset.json:98 (vinceaudio spec)
    (b".vbeam", 0x60000020),    # VinceLighthouseFix/patchset.json:6-8
    (b".vfogz", 0x60000020),    # VinceFogSortFix/patchset.json:9-10
    (b".vfogd", 0xC0000040),    # VinceFogSortFix/patchset.json:62-64
    (b".vwide", 0x60000020),    # ultrawide/ultrawide_patch.py (1.4 T2)
    (b".vnrfix", 0x60000020),   # Charmainefix/tools/patch_narration.py:167-168
    (b".vwalk", 0x60000020),    # Cutscenewalk/tools/patch_walkout.py:173-174
    (b".vcsfix", 0x60000020),   # VinceParticuleoverload/tools/patch_cutscenefix.py:165-166
]


def _imports(data):
    """[(dll, [(name|ordinal, thunk address)])] as pefile parses them."""
    try:
        pe = pefile.PE(data=data, fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
        out = [(e.dll, [(i.name if i.name else i.ordinal, i.address) for i in e.imports])
               for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])]
        pe.close()
        return out
    except pefile.PEFormatError as e:
        raise Refused(f"layout: imports do not parse: {e}") from None


def check_layout(stock, final, done):
    """The final image's layout dict; Refused on any departure from the expected layout."""
    def need(ok, what):
        if not ok:
            raise Refused(f"layout: {what}")

    moves = [s for s, _, _, _ in done if s["adapter"] == "header_move"]
    need(len(moves) == 1, f"{len(moves)} header_move stages")
    s, f = pe_info(stock), pe_info(final)
    need(f["lfanew"] == 0x40, f"e_lfanew {f['lfanew']:#x}")
    need(len(s["secs"]) == 7 and f["count"] == 7 + len(ADDED), f"section count {s['count']} -> {f['count']}")
    for i in range(7):
        need(final[f["table"] + 40 * i:f["table"] + 40 * i + 40] == stock[s["table"] + 40 * i:s["table"] + 40 * i + 40],
             f"stock section header {i} changed")
    flags = [struct.unpack_from("<I", final, f["table"] + 40 * i + 36)[0] for i in range(f["count"])]
    got = [(sec[0], flags[7 + i]) for i, sec in enumerate(f["secs"][7:])]
    need(got == ADDED, f"added sections {[(n.decode(), hex(c)) for n, c in got]}")
    va = FIRST_ADDED_VA
    for name, vs, sva, _, _ in f["secs"][7:]:
        need(sva == va, f"{name.decode()} at {sva:#x}, expected {va:#x}")
        va = (sva + vs + 0xFFF) & ~0xFFF
    size_of_image = struct.unpack_from("<I", final, f["opt"] + 56)[0]
    need(size_of_image == va, f"SizeOfImage {size_of_image:#x}, sections end {va:#x}")
    for d in (1, 12):
        need(final[f["opt"] + 96 + 8 * d:f["opt"] + 104 + 8 * d] == stock[s["opt"] + 96 + 8 * d:s["opt"] + 104 + 8 * d],
             f"data directory {d} changed")
    irva, isize = struct.unpack_from("<II", stock, s["opt"] + 104)
    ioff = next(rp + irva - sva for _, vs, sva, rs, rp in s["secs"] if sva <= irva < sva + max(vs, rs))
    need(final[ioff:ioff + isize] == stock[ioff:ioff + isize], "import descriptor bytes changed")
    need(_imports(final) == _imports(stock), "parsed imports differ from stock")
    need(not any(n in (b".vaudio", b".vstm") for n, *_ in f["secs"]), "a .vaudio/.vstm section is present")
    by = {n: (vs, sva, rs, rp) for n, vs, sva, rs, rp in f["secs"]}
    links = []
    for rel, spec_rel in (("VinceLighthouseFix/install_patch.py", "VinceLighthouseFix/patchset.json"),
                          ("VinceFogSortFix/install_patch.py", "VinceFogSortFix/patchset.json")):
        spec = load_json(spec_rel)
        name = spec["section"]["name"].encode()
        vs, sva, _, rp = by[name]
        placed = IMAGE_BASE + sva
        need(final[rp:rp + vs] == module(rel).payload_at(spec, placed), f"{name.decode()} payload != payload_at")
        if "data" in spec:
            dname = spec["data"]["name"].encode()
            need(by[dname][1] == sva + spec["data"]["rva_offset"], f"{dname.decode()} not at {name.decode()} + rva_offset")
        links.append({"section": name.decode(), "link_va": spec["section"]["link_va"], "placed_va": placed})
    first_raw = min(rp for _, _, _, rs, rp in f["secs"] if rs)
    return {"e_lfanew": f["lfanew"], "size_of_headers": f["size_of_headers"], "slots_used": f["count"],
            "slots_capacity": (min(f["size_of_headers"], first_raw) - f["table"]) // 40,
            "sections": [{"name": n.decode(), "va": sva, "vsize": vs, "raw_ptr": rp, "raw_size": rs, "flags": c}
                         for (n, vs, sva, rs, rp), c in zip(f["secs"], flags)],
            "links": links}


def build_exe(stock_dir, out_dir):
    stock_dir, out_dir = Path(stock_dir).resolve(), Path(out_dir).resolve()
    if out_dir == stock_dir or stock_dir in out_dir.parents:
        raise ValueError("--out must not lie inside the oracle")
    stock = (stock_dir / "Vince.exe").read_bytes()
    done = compose(stock)
    final = set_checksum(done[-1][3])
    layout = check_layout(stock, final, done)
    stages = out_dir / "stages"
    stages.mkdir(parents=True, exist_ok=True)
    for f in stages.glob("[0-9][0-9]-*.exe"):
        f.unlink()
    build_own, steps = set(), []
    for step, name, before, after in done:
        diff = changed(before, after)
        if step["adapter"] == "header_move":
            mine, theirs = [], diff
        else:
            own = build_offsets(before) | build_offsets(after)
            mine = [i for i in diff if i not in own]
            theirs = [i for i in diff if i in own]
        build_own.update(theirs)
        (stages / name).write_bytes(after)
        steps.append({"component": step["component"], "sets": step.get("sets"), "stage": f"stages/{name}",
                      "sha256": hashlib.sha256(after).hexdigest(), "ranges": ranges(mine)})
    build_own.update(changed(done[-1][3], final))
    (out_dir / "Vince.exe").write_bytes(final)
    ownership = {"schema": SCHEMA, "oracle_sha256": hashlib.sha256(stock).hexdigest(),
                 "final_sha256": hashlib.sha256(final).hexdigest(), "steps": steps,
                 "build": ranges(sorted(build_own)), "layout": layout}
    (out_dir / "ownership.json").write_text(json.dumps(ownership, indent=1) + "\n", encoding="utf-8")
    return ownership


# ---- dll: the AIO core X3DAudio1_7.dll from the three 1.3 trees (aio/core/core.c) ----

CORE = HERE / "core"
_C = ["--target=i686-pc-windows-msvc", "-O2"]
_FREE = ["-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-mno-stack-arg-probe", "-Wall", "-Wextra"]
VA_FLAGS = _C + _FREE + ["-Werror"]  # "Xaudio2 rework/scripts/build.ps1":95; also core.c
# (tree, sources, its own builder's flags), each compiled with -DVINCE_AIO
DLL_TREES = [
    (ROOT / "VinceFix/release/github-1.3",
     ["dllmain.c", "common.c", "hooks.c", "audio.c", "settings.c", "present.c", "setup_info.c"],
     _C + _FREE),  # build.ps1:15
    (ROOT / "Xaudio2 rework/src/adapter", ["bridge_voice.c", "bridge_engine.c", "bridge_shadow.c", "loader.c"], VA_FLAGS),
    (ROOT / "VinceSteam/src", ["vincesteam.c"],
     _C + ["-msse2", "-fno-math-errno", "-fno-builtin", "-ffreestanding", "-fno-stack-protector",
           "-mno-stack-arg-probe", "-Wall", "-Wextra"]),  # build.sh:18-19
    (CORE, ["core.c", "halo.c"], VA_FLAGS),  # halo.c: 1.6 T4
]
THUNKS = ROOT / "VinceFix/release/github-1.3/thunks.s"
CORE_EXPORTS = {"X3DAudioCalculate": 1, "X3DAudioInitialize": 2}
DLL_CHARS = {"DYNAMIC_BASE": 0x40, "NX_COMPAT": 0x100}
NO_SEH = 0x400


def llvm_version_lines(bin_dir):
    """First --version line of clang and lld-link in bin_dir."""
    out = []
    for tool in ("clang.exe", "lld-link.exe"):
        run = subprocess.run([str(bin_dir / tool), "--version"], capture_output=True, text=True)
        out.append((run.stdout.splitlines() or [""])[0])
    return out


def check_llvm(bin_dir):
    """ValueError unless clang and lld-link are the pinned LLVM (version and commit, aio/toolchain.py)."""
    tc = module("aio/toolchain.py")
    clang, lld = llvm_version_lines(bin_dir)
    for line, need in ((clang, f"clang version {tc.LLVM_VERSION} "), (lld, f"LLD {tc.LLVM_VERSION} ")):
        if need not in line or tc.LLVM_COMMIT not in line:
            raise ValueError(f"LLVM must be {tc.LLVM_VERSION} ({tc.LLVM_COMMIT}); {bin_dir} reports {line!r}")


def _tool(bin_dir, name, args, cwd=None):
    run = subprocess.run([str(bin_dir / name)] + [str(a) for a in args], capture_output=True, text=True, cwd=cwd)
    if run.returncode:
        raise Refused(f"{name} failed ({run.returncode}): {(run.stdout + run.stderr).strip()[-2000:]}")


def allowed_imports():
    lines = (CORE / "imports-allowed.txt").read_text(encoding="utf-8").splitlines()
    return sorted({ln.split("#", 1)[0].strip() for ln in lines} - {""})


def check_core(data):
    """Refused unless data is a PE32 DLL with exactly the core's exports, imports-allowed.txt's imports and the flags."""
    pe = pefile.PE(data=data)

    def need(ok, what):
        if not ok:
            raise Refused("core DLL: " + what)

    need(pe.FILE_HEADER.Machine == 0x14C and pe.FILE_HEADER.Characteristics & 0x2000 and
         pe.OPTIONAL_HEADER.Magic == 0x10B, "not a PE32 x86 DLL")
    chars = pe.OPTIONAL_HEADER.DllCharacteristics
    for name, bit in DLL_CHARS.items():
        need(chars & bit, f"DllCharacteristics {chars:#x} lacks {name}")
    seh_table = bool(getattr(pe, "DIRECTORY_ENTRY_LOAD_CONFIG", None) and
                     getattr(pe.DIRECTORY_ENTRY_LOAD_CONFIG.struct, "SEHandlerTable", 0))
    need(chars & NO_SEH or seh_table, f"DllCharacteristics {chars:#x}: no NO_SEH and no SEHandlerTable")
    exp = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
    got = {s.name.decode(): s.ordinal for s in exp.symbols} if exp else {}
    need(got == CORE_EXPORTS, f"exports {got}")
    need(all(s.forwarder is None for s in exp.symbols), "an export is a forwarder")
    need(not hasattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT"), "delay-load imports")
    imports = []
    for d in pe.DIRECTORY_ENTRY_IMPORT:
        for i in d.imports:
            need(i.name is not None, f"{d.dll.decode()} import by ordinal")
            imports.append(f"{d.dll.decode().lower()}!{i.name.decode()}")
    want = allowed_imports()
    need(sorted(imports) == want, f"imports differ from aio/core/imports-allowed.txt: extra "
         f"{sorted(set(imports) - set(want))}, missing {sorted(set(want) - set(imports))}")
    for p in (str(ROOT), str(ROOT).replace("\\", "/")):
        need(p.encode() not in data and p.encode("utf-16-le") not in data, f"holds the path {p}")
    return chars


def build_dll(out_dir):
    """Compile, link and check the core; write <out_dir>/X3DAudio1_7.dll and VinceAIO.ini. (sha256, DllCharacteristics)."""
    tc = module("aio/toolchain.py")
    bin_dir = tc.LLVM_BIN
    check_llvm(bin_dir)
    SCRATCH.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="dll-", dir=SCRATCH))
    try:
        libs = []
        for dll in ("kernel32", "user32", "ole32"):
            libs.append(work / f"{dll}.lib")
            _tool(bin_dir, "llvm-dlltool.exe", ["-m", "i386", "-k", "-d", CORE / f"{dll}.def", "-l", libs[-1]])
        objs = []
        for n, (tree, sources, flags) in enumerate(DLL_TREES):
            for src in sources:
                objs.append(work / f"{n}-{Path(src).stem}.obj")
                _tool(bin_dir, "clang.exe", flags + ["-DVINCE_AIO", "-c", src, "-o", objs[-1]], cwd=tree)
        objs.append(work / "thunks.obj")
        _tool(bin_dir, "clang.exe", ["--target=i686-pc-windows-msvc", "-c", THUNKS, "-o", objs[-1]])
        dll = work / "X3DAudio1_7.dll"
        _tool(bin_dir, "lld-link.exe", ["/dll", "/nodefaultlib", "/entry:DllMain", "/machine:x86", "/subsystem:windows",
                                        "/Brepro", f"/def:{CORE / 'exports.def'}", f"/out:{dll}"] + objs + libs)
        data = dll.read_bytes()
        chars = check_core(data)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "X3DAudio1_7.dll").write_bytes(data)
    (out_dir / "VinceAIO.ini").write_bytes((CORE / "VinceAIO.ini").read_bytes())
    return hashlib.sha256(data).hexdigest(), chars


WATER_NATIVE = "VinceWaterFix/tools/build_native.py"
WATER_NOTICES = "VinceWaterFix/third_party/minhook/LICENSE.txt"  # as VinceWaterFix/tools/package_aio.py:41-49


def build_water(out_dir=None):
    """Build the water d3d11.dll with build_native.build() (pinned LLVM, scratch under SCRATCH, removed after) and
    assert data_chain.json "water" sha256. {name: bytes} of d3d11.dll and VinceWaterFix.NOTICES.txt; written to
    out_dir when given."""
    pin = json.loads(DATA_CHAIN.read_text(encoding="utf-8"))["water"]
    bin_dir = module("aio/toolchain.py").LLVM_BIN
    check_llvm(bin_dir)
    SCRATCH.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="water-", dir=SCRATCH))
    try:
        data = module(WATER_NATIVE).build(work, bin_dir)
    except subprocess.CalledProcessError as e:
        raise Refused(f"water: {Path(str(e.cmd[0])).name} failed ({e.returncode})")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    got = hashlib.sha256(data).hexdigest()
    if got != pin["sha256"]:
        raise Refused(f"water d3d11.dll sha256 {got[:16]} is not the pinned {pin['sha256'][:16]} ({pin['reference']})")
    files = {"d3d11.dll": data, "VinceWaterFix.NOTICES.txt": (ROOT / WATER_NOTICES).read_bytes()}
    if out_dir is not None:
        Path(out_dir).mkdir(parents=True, exist_ok=True)
        for name, b in files.items():
            (Path(out_dir) / name).write_bytes(b)
    return files


DATA_CHAIN = HERE / "data_chain.json"
HOT ="VinceGateAnimFix/gate_anim_fix.py"  # its hot_entries/read_entry parse every PC HOT archive (C10 guards)


def hot_table(data):
    """[(name, entry)] in table order; refuses a layout the parser collapses (duplicate names)."""
    g = module(HOT)
    es = g.hot_entries(data)
    if len(es) != struct.unpack_from("<I", data, 24)[0]:
        raise ValueError("HOT archive: duplicate entry names")
    return sorted(es.items(), key=lambda kv: kv[1]["pos"])


def changed_entries(old, new, depth=0):
    """{nested name: sha256} of the decompressed entries that differ; refuses a changed table (names, order, packing).
    A size may change only with the content, so it shows as a changed (pinned) entry (pupilfix's .anim grows)."""
    g = module(HOT)
    a, b = hot_table(old), hot_table(new)
    if [(n, bool(e["packed"])) for n, e in a] != [(n, bool(e["packed"])) for n, e in b]:
        raise Refused("entry table differs from the input's (names, order, packing)")
    out = {}
    for (name, ea), (_, eb) in zip(a, b):
        try:
            x, y = g.read_entry(old, ea), g.read_entry(new, eb)
        except zlib.error as e:
            raise ValueError(f"{name}: {e}")
        if x == y:
            continue
        if depth < 2 and x[:4] == y[:4] == b"HOT ":
            out.update({f"{name}/{k}": v for k, v in changed_entries(x, y, depth + 1).items()})
        else:
            out[name] = hashlib.sha256(y).hexdigest()
    return out


def data_rel(rel):
    p = Path(rel)
    if p.is_absolute() or p.drive or ".." in p.parts or not p.parts:
        raise ValueError(f"data_chain.json: path {rel!r} is not relative")
    return p


def run_step(step, data, stock_dir):
    """One data step (aio/data_chain.json or aio/options_chain.json) on its input bytes in memory:
    (output, input sha256, output sha256, changed entries or None, pin text); Refused on any pin miss.
    fn(input, *extra, *args) -> (bytes|None, report), or with "named": fn(input path, input) -> bytes."""
    comp, rel, zv = step["component"], step["input"], zlib.ZLIB_RUNTIME_VERSION
    in_sha = hashlib.sha256(data).hexdigest()
    if in_sha != step["input_sha256"]:
        raise Refused(f"{comp} {rel}: input sha256 {in_sha[:16]} is not the pinned {step['input_sha256'][:16]}")
    report = {}
    if step["fn"] == "copy":
        new = data
    else:
        path, name = step["fn"].rsplit(":", 1)
        if step.get("named"):
            new = getattr(module(path), name)(rel, data)
        else:
            extra = [(stock_dir / data_rel(x)).read_bytes() for x in step.get("extra", [])]
            result = getattr(module(path), name)(data, *extra, *step.get("args", []))
            new, report = result[0], result[1] if isinstance(result[1], dict) else {}
        if new is None:
            raise Refused(f"{comp} {rel}: the step changed nothing ({report.get('state')})")
    pin, out_sha = step["pin"], hashlib.sha256(new).hexdigest()
    for k, v in pin.get("report", {}).items():
        if report.get(k) != v:
            raise Refused(f"{comp} {rel}: report {k} = {report.get(k)!r}, pinned {v!r} ({pin['reference']})")
    entries = changed_entries(data, new) if new[:4] == b"HOT " else None
    if "entries" in pin and entries != pin["entries"]:
        raise Refused(f"{comp} {rel}: changed entries {entries} are not the pinned {pin['entries']}")
    if "sha256" not in pin:
        result = f"entry-level vs {pin['reference']}" + "".join(f"; {k}" for k in sorted(pin.get("report", {})))
    elif out_sha == pin["sha256"]:
        result = f"whole-file {out_sha[:8]}" + (f" (ours zlib {zv}, reference zlib {pin['zlib']})" if "zlib" in pin else "")
    elif "entries" in pin:
        result = (f"entry-level; whole file {out_sha[:8]} != {pin['sha256'][:8]} "
                  f"(ours zlib {zv}, reference zlib {pin.get('zlib', 'unrecorded')})")
    else:
        raise Refused(f"{comp} {rel}: output sha256 {out_sha[:16]} is not the pinned {pin['sha256'][:16]}")
    return new, in_sha, out_sha, entries, result


def drop_unheld(out_dir, comps, held):
    """Remove <out>/held/<comp>/ left by an earlier build for each of comps no longer release_held (1.7 T3), and
    <out>/held/ when that leaves it empty."""
    for comp in sorted(set(comps)):
        if held.get(comp) is False and (out_dir / "held" / comp).exists():  # exe variants carry "exe_chain"
            shutil.rmtree(out_dir / "held" / comp)
    if (out_dir / "held").is_dir() and not any((out_dir / "held").iterdir()):
        (out_dir / "held").rmdir()


def build_data(stock_dir, out_dir):
    """Run aio/data_chain.json on the oracle's bytes in memory, assert every pin, then write the outputs and
    ownership-data.json (nothing is written when a step refuses)."""
    stock_dir, out_dir = Path(stock_dir).resolve(), Path(out_dir).resolve()
    if out_dir == stock_dir or stock_dir in out_dir.parents:
        raise ValueError("--out must not lie inside the oracle")
    chain = json.loads(DATA_CHAIN.read_text(encoding="utf-8"))
    if chain.get("schema") != SCHEMA:
        raise ValueError("data_chain.json: schema")
    held = {c["id"]: bool(c["release_held"]) for c in load_json("aio/components.json")["components"]}
    zv = zlib.ZLIB_RUNTIME_VERSION
    files, records = {}, []
    for step in chain["steps"]:
        comp, rel = step["component"], step["input"]
        if comp not in held:
            raise ValueError(f"data_chain.json: unknown component {comp}")
        src = (ROOT if step.get("from") == "repo" else stock_dir) / data_rel(rel)
        data = src.read_bytes()
        new, in_sha, out_sha, entries, result = run_step(step, data, stock_dir)
        dest = (Path("held", comp) if held[comp] else Path()) / data_rel(step["output"])
        if dest.as_posix() in files:
            raise ValueError(f"data_chain.json: two steps write {dest.as_posix()}")
        files[dest.as_posix()] = new
        records.append({"component": comp, "input": rel, "input_from": step.get("from", "stock"),
                        "input_sha256": in_sha, "output": dest.as_posix(), "output_sha256": out_sha,
                        "changed_entries": sorted(entries) if entries is not None else None,
                        "held": held[comp], "pin": result})
    water, wpin = build_water(), chain["water"]
    for name, new in water.items():
        dest = (Path("held", "water") if held["water"] else Path()) / name
        if dest.as_posix() in files:
            raise ValueError(f"data_chain.json: a step writes the water output {dest.as_posix()}")
        files[dest.as_posix()] = new
        src = WATER_NATIVE if name == "d3d11.dll" else WATER_NOTICES
        records.append({"component": "water", "input": src, "input_from": "repo",
                        "input_sha256": hashlib.sha256((ROOT / src).read_bytes()).hexdigest(),
                        "output": dest.as_posix(), "output_sha256": hashlib.sha256(new).hexdigest(),
                        "changed_entries": None, "held": held["water"],
                        "pin": (f"whole-file {wpin['sha256'][:8]} ({wpin['reference']})" if name == "d3d11.dll"
                                else f"copy of {WATER_NOTICES}")})
    drop_unheld(out_dir, [s["component"] for s in chain["steps"]] + ["water"], held)
    for rel, new in sorted(files.items()):
        (out_dir / rel).parent.mkdir(parents=True, exist_ok=True)
        (out_dir / rel).write_bytes(new)
    own = {"schema": SCHEMA, "zlib_runtime_version": zv, "outputs": sorted(records, key=lambda r: r["output"])}
    (out_dir / "ownership-data.json").write_text(json.dumps(own, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    return own


OPTIONS_CHAIN = HERE / "options_chain.json"
OPTIONS = ("voodoopowerfix", "firebug", "retexture")  # set names and file order


def option_set(opts):
    """Canonical [ids] and set name ("none" or ids joined by + in OPTIONS order) of an option list."""
    if len(set(opts)) != len(opts) or not set(opts) <= set(OPTIONS):
        raise ValueError(f"options_chain.json: option set {opts}")
    ids = [o for o in OPTIONS if o in opts]
    return ids, "+".join(ids) or "none"


def pin_ranges(spec, comps):
    """The range pins named by "<component>.<key>[.<key>...][:<range name>]" in aio/components.json."""
    path, _, only = spec.partition(":")
    comp, *keys = path.split(".")
    node = comps[comp]["pins"]
    for k in keys:
        node = node[k]
    got = [r for r in node if not only or r["name"] == only]
    if not got:
        raise ValueError(f"options_chain.json: no range pin {spec}")
    return got


def option_exes(stock, exe, comps):
    """[(variant, final bytes, pin text)]: compose + CheckSum + check_layout as build_exe, then each variant's fn on
    the composed exe; Refused unless the base, apply, final and range pins (aio/components.json) all hold."""
    done = compose(stock)
    composed = set_checksum(done[-1][3])
    check_layout(stock, composed, done)
    base = hashlib.sha256(composed).hexdigest()
    if base != exe["base_sha256"]:
        raise Refused(f"composed exe sha256 {base[:16]} is not the pinned {exe['base_sha256'][:16]}")
    out = []
    for v in exe["variants"]:
        name = option_set(v["options"])[1]
        if v["fn"] == "copy":
            applied = composed
        else:
            path, fn = v["fn"].rsplit(":", 1)
            try:
                applied = getattr(module(path), fn)(composed, *v.get("args", []))
            except ValueError as e:
                raise Refused(f"exe {name}: {e}") from None
        got = hashlib.sha256(applied).hexdigest()
        if got != v["apply_sha256"]:
            raise Refused(f"exe {name}: applied sha256 {got[:16]} is not the pinned {v['apply_sha256'][:16]}")
        final = set_checksum(applied)
        got = hashlib.sha256(final).hexdigest()
        if got != v["sha256"]:
            raise Refused(f"exe {name}: sha256 {got[:16]} is not the pinned {v['sha256'][:16]}")
        names = []
        for spec in v.get("ranges", []):
            for r in pin_ranges(spec, comps):
                off = int(r["offset"], 16)
                if hashlib.sha256(final[off:off + r["size"]]).hexdigest() != r["sha256"]:
                    raise Refused(f"exe {name}: range {r['name']} is not the pin {spec}")
                names.append(r["name"])
        for spec in v.get("unchanged", []):
            for r in pin_ranges(spec, comps):
                off = int(r["offset"], 16)
                if final[off:off + r["size"]] != composed[off:off + r["size"]]:
                    raise Refused(f"exe {name}: range {r['name']} differs from the composed exe")
                names.append(r["name"] + "=composed")
        out.append((v, final, f"whole-file {got[:8]}" + (f"; ranges {', '.join(names)}" if names else "")))
    return out


def option_data(steps, stock_dir, held):
    """Run the options_chain.json data steps in memory: ({(set name, game path): bytes}, [record]). Input: stock,
    the "over" set's output on the same path, or the always-on bytes when the path is an aio/data_chain.json output
    (that step run in memory, C0042 layering); Refused on any pin miss."""
    always = {}
    for s in json.loads(DATA_CHAIN.read_text(encoding="utf-8"))["steps"]:
        always.setdefault(data_rel(s["output"]).as_posix(), s)
    built, records = {}, []
    for step in steps:
        comp, rel = step["component"], data_rel(step["input"]).as_posix()
        if comp not in held:
            raise ValueError(f"options_chain.json: unknown component {comp}")
        ids, name = option_set(step["options"])
        if not ids:
            raise ValueError(f"options_chain.json: {comp} {rel}: a data step needs options")
        if "over" in step:
            key = (option_set(step["over"])[1], rel)
            if key not in built:
                raise ValueError(f"options_chain.json: {comp} {rel}: no earlier {key[0]} output to build over")
            data, frm = built[key], key[0]
        elif rel in always:
            a = always[rel]
            src = (ROOT if a.get("from") == "repo" else stock_dir) / data_rel(a["input"])
            data, frm = run_step(a, src.read_bytes(), stock_dir)[0], "always-on"
        else:
            data, frm = (stock_dir / rel).read_bytes(), "stock"
        new, in_sha, out_sha, entries, result = run_step(step, data, stock_dir)
        game = data_rel(step["output"]).as_posix()
        if (name, game) in built:
            raise ValueError(f"options_chain.json: two {name} steps write {game}")
        built[(name, game)] = new
        dest = (Path("held", comp) if held[comp] else Path("options", name)) / game
        records.append({"output": dest.as_posix(), "game_path": game, "sha256": out_sha, "options": ids,
                        "component": comp, "input": rel, "input_from": frm, "input_sha256": in_sha,
                        "changed_entries": sorted(entries) if entries is not None else None,
                        "held": held[comp], "pin": result})
    return built, records


def build_options(stock_dir, out_dir):
    """Build every option file of aio/options_chain.json in memory and assert every pin, then replace <out>/options/,
    write held files under <out>/held/<component>/ and <out>/ownership-options.json (nothing when anything refuses)."""
    stock_dir, out_dir = Path(stock_dir).resolve(), Path(out_dir).resolve()
    if out_dir == stock_dir or stock_dir in out_dir.parents:
        raise ValueError("--out must not lie inside the oracle")
    chain = json.loads(OPTIONS_CHAIN.read_text(encoding="utf-8"))
    if chain.get("schema") != SCHEMA:
        raise ValueError("options_chain.json: schema")
    comps = {c["id"]: c for c in load_json("aio/components.json")["components"]}
    held = {k: bool(c["release_held"]) for k, c in comps.items()}
    stock = (stock_dir / "Vince.exe").read_bytes()
    built, records = {}, []
    for v, final, pin in option_exes(stock, chain["exe"], comps):
        ids, name = option_set(v["options"])
        if (name, "Vince.exe") in built:
            raise ValueError(f"options_chain.json: two {name} exe variants")
        built[(name, "Vince.exe")] = final
        records.append({"output": f"options/{name}/Vince.exe", "game_path": "Vince.exe",
                        "sha256": hashlib.sha256(final).hexdigest(), "options": ids, "component": v["component"],
                        "input": "Vince.exe", "input_from": "always-on", "input_sha256": chain["exe"]["base_sha256"],
                        "changed_entries": None, "held": False, "pin": pin})
    data, recs = option_data(chain["steps"], stock_dir, held)
    built.update(data)
    records += recs
    by_key = {(option_set(r["options"])[1], r["game_path"]): r for r in records}
    touch = {}
    for r in records:
        touch.setdefault(r["game_path"], set()).update(r["options"])
    combos = []
    for mask in range(1 << len(OPTIONS)):
        on = [o for i, o in enumerate(OPTIONS) if mask >> i & 1]
        files = []
        for game in sorted(touch):
            ids, name = option_set([o for o in on if o in touch[game]])
            r = by_key.get((name, game))
            if r is None:
                if ids:
                    raise ValueError(f"options_chain.json: combination {option_set(on)[1]} needs {game} "
                                     f"built for {name}: no step")
                continue
            files.append({"game_path": game, "output": r["output"], "sha256": r["sha256"], "owning": ids,
                          "held": r["held"]})
        combos.append({"options": on, "set": option_set(on)[1], "files": files})
    if len({r["output"] for r in records}) != len(records):
        raise ValueError("options_chain.json: two steps write the same output")
    if (out_dir / "options").exists():
        shutil.rmtree(out_dir / "options")
    drop_unheld(out_dir, [r["component"] for r in records], held)
    for r in records:
        dest = out_dir / r["output"]
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(built[(option_set(r["options"])[1], r["game_path"])])
    own = {"schema": SCHEMA, "zlib_runtime_version": zlib.ZLIB_RUNTIME_VERSION,
           "files": sorted(records, key=lambda r: r["output"]), "combinations": combos}
    (out_dir / "ownership-options.json").write_text(json.dumps(own, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    return own


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    exe = sub.add_parser("exe", help="compose the exe chain onto stock")
    exe.add_argument("--stock", type=Path, required=True, help="oracle game folder holding Vince.exe (read only)")
    exe.add_argument("--out", type=Path, default=ROOT / "build" / "aio")
    dll = sub.add_parser("dll", help="build the AIO core X3DAudio1_7.dll")
    dll.add_argument("--out", type=Path, default=ROOT / "build" / "aio")
    dat = sub.add_parser("data", help="build the data outputs of aio/data_chain.json from stock")
    dat.add_argument("--stock", type=Path, required=True, help="oracle game folder (read only)")
    dat.add_argument("--out", type=Path, default=ROOT / "build" / "aio")
    opt = sub.add_parser("options", help="build the option files of aio/options_chain.json from stock")
    opt.add_argument("--stock", type=Path, required=True, help="oracle game folder (read only)")
    opt.add_argument("--out", type=Path, default=ROOT / "build" / "aio")
    args = parser.parse_args(argv)
    if args.cmd == "options":
        try:
            own = build_options(args.stock, args.out)
        except Refused as e:
            print("refused:", e)
            return 1
        except (OSError, ValueError) as e:
            print("error:", e)
            return 2
        print("zlib", own["zlib_runtime_version"])
        for r in own["files"]:
            print(f"{r['output']:80} {r['sha256'][:16]} {r['component']}: {r['pin']}")
        for c in own["combinations"]:
            print(f"combination {c['set']}: {len(c['files'])} files")
        return 0
    if args.cmd == "data":
        try:
            own = build_data(args.stock, args.out)
        except Refused as e:
            print("refused:", e)
            return 1
        except (OSError, ValueError) as e:
            print("error:", e)
            return 2
        print("zlib", own["zlib_runtime_version"])
        for r in own["outputs"]:
            print(f"{r['output']:70} {r['output_sha256'][:16]} {r['component']}: {r['pin']}")
        return 0
    if args.cmd == "dll":
        try:
            digest, chars = build_dll(args.out.resolve())
        except Refused as e:
            print("refused:", e)
            return 1
        except (OSError, ValueError) as e:
            print("error:", e)
            return 2
        print(f"DllCharacteristics {chars:#x}")
        print("X3DAudio1_7.dll", digest)
        print("VinceAIO.ini", hashlib.sha256((CORE / "VinceAIO.ini").read_bytes()).hexdigest())
        return 0
    try:
        own = build_exe(args.stock, args.out)
    except Refused as e:
        print("refused:", e)
        return 1
    except (OSError, ValueError) as e:
        print("error:", e)
        return 2
    for s in own["steps"]:
        print(f"{s['stage']:40} {s['sha256'][:16]} {len(s['ranges'])} ranges")
    lay = own["layout"]
    print(f"e_lfanew {lay['e_lfanew']:#x}  SizeOfHeaders {lay['size_of_headers']:#x}  "
          f"slots {lay['slots_used']}/{lay['slots_capacity']}")
    for c in lay["sections"]:
        print(f"  {c['name']:8} va {c['va']:#08x} vsize {c['vsize']:#08x} raw {c['raw_ptr']:#08x}/{c['raw_size']:#08x}"
              f" flags {c['flags']:#010x}")
    for k in lay["links"]:
        print(f"  {k['section']:8} link_va {k['link_va']:#x} -> placed {k['placed_va']:#x}")
    print("Vince.exe", own["final_sha256"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
