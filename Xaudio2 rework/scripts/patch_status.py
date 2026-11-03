"""Read-only status of the known Vince.exe patches: A (prepare_patch guards), B (tail fix), C (stereo music fix),
D (end fix), loader (VinceAudio import).

Usage: python -I scripts/patch_status.py <Vince.exe>
Prints one line per item (each site: original, patched or foreign), then `A: present|absent|partial`, `B: ...`,
`C: ...`, `D: ...`, `loader: ...`. Item `loader install check: pass|fail (...)` = install_check(), the loader.c va_install emulation. Before `A:`, one `other section <name>: present` per section (table order) that is neither in
VANILLA_SECTIONS nor one of the spec's sections. Never writes; every site, signature, section name, reloc and the
image base come from the patch spec (load_spec(): build/patchset/patchset.json if fresh, else built in memory;
docs/ops/patchset.md). stereo_fix() parses the constants of patches/VoodooVince_StereoMusicFix.bat for the spec.
"""
import json
import re
import runpy
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATCHES = ROOT / "patches"
IMAGE_BASE = 0x400000
SPEC_PATH = ROOT / "build" / "patchset" / "patchset.json"
SPEC_INPUTS = ("scripts/patchset.py", "scripts/patch_status.py", "patches/prepare_patch.py",
               "patches/prepare_recovery.py", "patches/vince_audio_tail_fix.py", "patches/VoodooVince_StereoMusicFix.bat",
               "patches/vince_audio_end_fix.py")
# section table of f7e08e9b, provisional vanilla until Steam verify (2.3 T4)
VANILLA_SECTIONS = ('.text', '.rdata', '.data', '.gfids', '.tls', '.rsrc', '.reloc')
_SPEC = None


def _sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    opt = pe + 24
    count = struct.unpack_from("<H", data, pe + 6)[0]
    table = opt + struct.unpack_from("<H", data, pe + 20)[0]
    out = []
    for i in range(count):
        at = table + i * 40
        name = data[at:at + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, rva, rsize, raw = struct.unpack_from("<IIII", data, at + 8)
        out.append((name, rva, max(vsize, rsize), raw, rsize))
    return opt, out


def _offset(sections, rva):
    for _, start, _, raw, rsize in sections:
        if start <= rva < start + rsize:
            return raw + rva - start
    raise ValueError(f"RVA 0x{rva:x} is not backed by file data")


def _imports(data, opt, sections):
    names = []
    pos = _offset(sections, struct.unpack_from("<I", data, opt + 104)[0])
    while any(data[pos:pos + 20]):
        name_rva = struct.unpack_from("<I", data, pos + 12)[0]
        at = _offset(sections, name_rva)
        names.append(data[at:data.index(0, at)].decode("ascii", "replace"))
        pos += 20
    return names


def stereo_fix():
    """Constants of patches/VoodooVince_StereoMusicFix.bat, parsed as data (the .bat is never run)."""
    text = (PATCHES / "VoodooVince_StereoMusicFix.bat").read_text(encoding="utf-8")
    piece = r"(?:'[0-9a-fA-F]*'|\('[0-9a-fA-F]+'\s*\*\s*\d+\))"

    def num(name):
        return int(re.search(rf"\${name}\s*=\s*(0x[0-9A-Fa-f]+)", text).group(1), 16)

    def hexbytes(name):
        expr = re.search(rf"\${name}\s*=\s*({piece}(?:\s*\+\s*{piece})*)", text).group(1)
        return bytes.fromhex("".join(a or b * int(n) for a, b, n in
                                     re.findall(r"'([0-9a-fA-F]+)'(?!\s*\*)|\('([0-9a-fA-F]+)'\s*\*\s*(\d+)\)", expr)))

    c = {"CodeRva": num("CodeRva"), "CodeOrig": hexbytes("CodeOrigHex"), "CodeNew": hexbytes("CodeNewHex"),
         "CtxBefore": (num("CtxBeforeRva"), hexbytes("CtxBeforeHex")),
         "CtxAfter": (num("CtxAfterRva"), hexbytes("CtxAfterHex")),
         "OldRelocs": [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+",
                                                       re.search(r"\$OldRelocs\s*=\s*@\(([^)]*)\)", text).group(1))],
         "NewReloc": num("NewReloc")}
    n = len(c["CodeNew"])
    assert len(c["CodeOrig"]) == n == c["CtxAfter"][0] - c["CodeRva"] and len(c["OldRelocs"]) == 15, "stereo fix .bat"
    return c


def _c_off(sections, rva, n):
    # .bat RvaToOff: rva..rva+n within one section's VA..VA+RawSize
    for _, start, _, raw, rsize in sections:
        if start <= rva and rva + n <= start + rsize:
            return raw + rva - start
    raise ValueError(f"RVA 0x{rva:x}+{n} is not backed by file data")


def c_code_at(data):
    """File offset of patch C's code; ValueError unless ImageBase is 0x400000 and both context runs match."""
    c = stereo_fix()
    opt, sections = _sections(data)
    if struct.unpack_from("<I", data, opt + 28)[0] != IMAGE_BASE:
        raise ValueError("ImageBase is not 0x400000")
    for rva, ctx in (c["CtxBefore"], c["CtxAfter"]):
        at = _c_off(sections, rva, len(ctx))
        if data[at:at + len(ctx)] != ctx:
            raise ValueError(f"context at RVA 0x{rva:X} differs")
    return _c_off(sections, c["CodeRva"], len(c["CodeNew"]))


def c_reloc_block(data, page=None):
    """(file offset, rva, slot count, page, entry RVAs) of the DataDirectory[5] block for `page` (default patch C's
    page); ValueError if there is none or it holds an entry that is neither HIGHLOW nor zero."""
    if page is None:
        page = stereo_fix()["CodeRva"] & ~0xFFF
    opt, sections = _sections(data)
    rva, size = struct.unpack_from("<II", data, opt + 136)
    if not size:
        raise ValueError("no relocation table")
    first = off = _c_off(sections, rva, 8)
    while off + 8 <= first + size:
        va, n = struct.unpack_from("<II", data, off)
        if n < 8:
            break
        if va == page:
            entries = []
            for (v,) in struct.iter_unpack("<H", data[off + 8:off + n]):
                if v >> 12 == 3:
                    entries.append(page + (v & 0xFFF))
                elif v:
                    raise ValueError(f"relocation entry 0x{v:04X} in page 0x{page:X} is not HIGHLOW")
            return off, rva + off - first, (n - 8) // 2, page, entries
        off += n
    raise ValueError(f"no relocation block for page 0x{page:X}")


def install_check(data):
    """Emulate src/adapter/loader.c va_install on the mapped image: (True, 'pass') or (False, reason)."""
    try:
        if data[:2] != b"MZ":
            return False, "no MZ"
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        opt = pe + 24
        if data[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", data, opt)[0] != 0x10B:
            return False, "not a PE32 image"
        size_image, size_headers = struct.unpack_from("<II", data, opt + 56)
        if size_image < 0x87A000:
            return False, f"SizeOfImage 0x{size_image:X} < 0x87A000"
        image = bytearray(size_image)
        image[:size_headers] = data[:size_headers]
        count = struct.unpack_from("<H", data, pe + 6)[0]
        table = opt + struct.unpack_from("<H", data, pe + 20)[0]
        for i in range(count):
            vsize, rva, rsize, raw = struct.unpack_from("<IIII", data, table + i * 40 + 8)
            n = max(0, min(rsize, size_image - rva))
            image[rva:rva + n] = data[raw:raw + n]
        if image[0x5CDBF:0x5CDBF + 6] != bytes.fromhex("ff52208b4b10"):
            return False, "no source-voice call at RVA 0x5CDBF"
        slot = 0x2D63D0
        pos = struct.unpack_from("<I", image, opt + 104)[0]
        while any(image[pos:pos + 20]):
            name_rva, iat = struct.unpack_from("<II", image, pos + 12)
            if image[name_rva:image.index(0, name_rva)].lower() == b"ole32.dll":
                n = 0
                while struct.unpack_from("<I", image, iat + 4 * n)[0]:
                    n += 1
                if not (iat <= slot < iat + 4 * n and (slot - iat) % 4 == 0):
                    return False, f"RVA 0x{slot:X} not in the ole32.dll IAT"
                value = struct.unpack_from("<I", image, slot)[0]
                if value >> 31 or image[value + 2:image.index(0, value + 2)] != b"CoCreateInstance":
                    return False, f"RVA 0x{slot:X} does not name CoCreateInstance"
                return True, "pass"
            pos += 20
        return False, "no ole32.dll import"
    except (IndexError, ValueError, struct.error) as e:
        return False, f"malformed image ({e})"


def load_spec():
    """The patch spec as a dict: build/patchset/patchset.json when its mtime >= every input's, else
    patchset.build_spec() parsed in memory (never written). Loaded lazily (patchset.py runpy-loads this module via
    prepare_recovery) and cached."""
    global _SPEC
    if _SPEC is None:
        mtime = SPEC_PATH.stat().st_mtime if SPEC_PATH.exists() else None
        if mtime is not None and all(mtime >= (ROOT / p).stat().st_mtime for p in SPEC_INPUTS):
            _SPEC = json.loads(SPEC_PATH.read_bytes())
        else:
            _SPEC = json.loads(runpy.run_path(str(ROOT / "scripts" / "patchset.py"))["build_spec"]())
    return _SPEC


def _site_state(site, read, by_name):
    """original (== before), patched (== after; templated: == template outside fixup fields and every rel32 fixup
    lands in its named section), else foreign."""
    n = site["length"]
    cur = read(site["rva"], n)
    if cur == bytes.fromhex(site["before"]):
        return "original"
    if "after" in site:
        return "patched" if cur == bytes.fromhex(site["after"]) else "foreign"
    tmpl, fixups = bytes.fromhex(site["after_template"]), site["fixups"]
    if len(cur) != n:
        return "foreign"
    masked = bytearray(cur)
    for f in fixups:
        masked[f["at"]:f["at"] + 4] = bytes(4)
    if bytes(masked) != tmpl:
        return "foreign"
    for f in fixups:
        sec = by_name.get(f["target"].get("section"))
        if f["kind"] != "rel32" or not sec:
            return "foreign"
        target = site["rva"] + f["at"] + 4 + struct.unpack_from("<i", cur, f["at"])[0]
        if not sec[1] <= target < sec[1] + sec[2]:
            return "foreign"
    return "patched"


def _aggregate(states):
    return ("present" if all(s == "patched" for s in states)
            else "absent" if all(s == "original" for s in states) else "partial")


def status(path):
    """Return {'items': [(label, state)], 'A': ..., 'B': ..., 'C': ..., 'D': ..., 'loader': ..., 'other': [names]}; states as
    in the module doc."""
    spec = load_spec()
    pat = {p["id"]: p for p in spec["patches"]}
    image_base = spec["image_base"]
    data = Path(path).read_bytes()
    opt, sections = _sections(data)
    by_name = {s[0]: s for s in sections}
    items = []

    def read(rva, n):
        at = _offset(sections, rva)
        return data[at:at + n]

    # A: its section and its sites (detours into the section, NOP run).
    vacfix_name = pat["A"]["sections"][0]["name"]
    vacfix = vacfix_name in by_name
    items.append((f"A section {vacfix_name}", "present" if vacfix else "absent"))
    a_states = []
    for site in pat["A"]["sites"]:
        st = _site_state(site, read, by_name)
        a_states.append(st)
        items.append((f"A 0x{image_base + site['rva']:X}", st))
    if vacfix and all(s == "patched" for s in a_states):
        a = "present"
    elif not vacfix and all(s == "original" for s in a_states):
        a = "absent"
    else:
        a = "partial"

    # B: tail-fix site and cave.
    b_states = []
    for label, site in zip(("site", "cave"), pat["B"]["sites"]):
        st = _site_state(site, read, by_name)
        b_states.append(st)
        items.append((f"B {label} 0x{image_base + site['rva']:X}", st))
    bstat = _aggregate(b_states)

    # D: end-fix sites and caves.
    d_states = []
    d_labels = ("main site", "main cave", "go site", "go cave", "post site", "post cave")
    for label, site in zip(d_labels, pat["D"]["sites"]):
        st = _site_state(site, read, by_name)
        d_states.append(st)
        items.append((f"D {label} 0x{image_base + site['rva']:X}", st))
    dstat = _aggregate(d_states)

    # C: stereo music fix code and its relocation block (ImageBase or context mismatch -> code foreign).
    c_site, c_rel = pat["C"]["sites"][0], pat["C"]["relocs"][0]
    start, end = c_site["rva"], c_site["rva"] + c_site["length"]
    try:
        if struct.unpack_from("<I", data, opt + 28)[0] != image_base:
            raise ValueError("ImageBase differs")
        for s in pat["C"]["signatures"]:
            if s["rva"] == start:
                continue
            ctx = bytes.fromhex(s["hex"])
            at = _c_off(sections, s["rva"], len(ctx))
            if data[at:at + len(ctx)] != ctx:
                raise ValueError("context differs")
        at = _c_off(sections, start, c_site["length"])
        cur = data[at:at + c_site["length"]]
        code = ("original" if cur == bytes.fromhex(c_site["before"])
                else "patched" if cur == bytes.fromhex(c_site["after"]) else "foreign")
    except (ValueError, struct.error):
        code = "foreign"
    try:
        inside = {r for r in c_reloc_block(data, c_rel["page"])[4] if r + 4 > start and r < end}
        relocs = ("original" if inside == set(c_rel["out"])
                  else "patched" if inside == set(c_rel["in"]) else "foreign")
    except (ValueError, struct.error):
        relocs = "foreign"
    items.append((f"C code 0x{image_base + start:X}", code))
    items.append((f"C relocs page 0x{c_rel['page']:X}", relocs))
    cstat = ("present" if code == relocs == "patched" else "absent" if code == relocs == "original" else "partial")

    # loader: its section and the import descriptor of the DLL named in its payload (fixup label `dll`).
    lsec = pat["loader"]["sections"][0]
    off = next(f["target"]["offset"] for f in lsec["fixups"] if f["target"].get("label") == "dll")
    payload = bytes.fromhex(lsec["payload"])
    dll = payload[off:payload.index(0, off)].decode("ascii")
    vaudio = lsec["name"] in by_name
    imported = any(n.lower() == dll.lower() for n in _imports(data, opt, sections))
    items.append((f"loader section {lsec['name']}", "present" if vaudio else "absent"))
    items.append((f"loader import {dll}", "present" if imported else "absent"))
    loader = "present" if vaudio and imported else "absent" if not (vaudio or imported) else "partial"
    ok, why = install_check(data)
    items.append(("loader install check", "pass" if ok else f"fail ({why})"))
    ours = {sec["name"] for p in spec["patches"] for sec in p["sections"]}
    other = [s[0] for s in sections if s[0] not in VANILLA_SECTIONS and s[0] not in ours]
    return {"items": items, "A": a, "B": bstat, "C": cstat, "D": dstat, "loader": loader, "other": other}


def main():
    if len(sys.argv) != 2:
        print("usage: patch_status.py <Vince.exe>")
        return 2
    s = status(sys.argv[1])
    for label, st in s["items"]:
        print(f"{label}: {st}")
    for name in s["other"]:
        print(f"other section {name}: present")
    for key in ("A", "B", "C", "D", "loader"):
        print(f"{key}: {s[key]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
