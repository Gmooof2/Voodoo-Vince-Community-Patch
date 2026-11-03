"""VinceSkinTexture, AIO builder: the retextured Vince skin and normal map written into each area's textures.hot.

Pure aio/build.py data-chain step: patch_textures_hot(data, area) -> (bytes or None, report). Stdlib only, writes no
files. Parses the archive with aio/hot.py, finds `vinceskin.dds` and `vinceskinn.1.00.dds` by name (case-insensitive,
one match each), and writes the committed .dds files over them in place: dds[:128] over the entry's DDS header,
dds[128:] over its stored pixel data, both at the stock size; nothing else changes (README.md:23-39). Refuses unless
the area is one of areas.txt, each entry is unpacked with raw == len(dds) and matches its per-area stock pins
(size, header_sha256, pixels_sha256: aio/components.json skintexture input entries, recorded by
docs/aio/evidence/1.3/skintexture/pins.py), and the .dds files are the pinned ones (S-skintexture-01). Entries that
already hold the new bytes are left alone; when both do, the result is None with state "applied".
"""
import hashlib
import importlib.util
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMPONENTS = HERE.parent / 'aio' / 'components.json'
HDR = 128
DDS_SHA256 = {
    'vinceskin.dds': '2124ddfa5a06d4f03c89530ec146251834e708fca68dcdd4094efb234c481c29',
    'vinceskinn.1.00.dds': 'ea34ce6a20a2f133367997a8ee2eff219ae2dfc9578fdd225c6a0edf48c12f9a',
}
NAMES = tuple(DDS_SHA256)


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


hot = _load('aio_hot', HERE.parent / 'aio' / 'hot.py')


class Refused(hot.Refused):
    """Not a stock skin entry of a known area, or not the pinned .dds files."""


def sha(b):
    return hashlib.sha256(b).hexdigest()


def areas():
    return [a.strip() for a in (HERE / 'areas.txt').read_text(encoding='ascii').splitlines() if a.strip()]


def hot_path(area):
    return f'vincedata/levels/{area}/textures.hot'


def stock_pins(area):
    """{name: {size, header_sha256, pixels_sha256}} for the area, from components.json skintexture input."""
    if area not in areas():
        raise Refused(f'{area!r} is not one of VinceSkinTexture/areas.txt')
    doc = json.loads(COMPONENTS.read_text(encoding='utf-8'))
    rec = next(c for c in doc['components'] if c['id'] == 'skintexture')
    inp = [e for e in rec['input'] if e['path'] == hot_path(area)]
    if len(inp) != 1:
        raise Refused(f'{area}: no stock pin for {hot_path(area)}')
    pins = {e['name']: e for e in inp[0].get('entries', [])}
    if set(pins) != set(NAMES):
        raise Refused(f'{area}: stock pins name {sorted(pins)}, expected {list(NAMES)}')
    return pins


def committed_dds():
    return {n: (HERE / 'textures' / n).read_bytes() for n in NAMES}


def patch_textures_hot(data, area, *, dds=None):
    """Return (new textures.hot bytes or None, report). dds = {name: bytes} overrides the committed files (tests)."""
    pins = stock_pins(area)
    dds = committed_dds() if dds is None else dds
    for n in NAMES:
        if sha(dds[n]) != DDS_SHA256[n]:
            raise Refused(f'{n}: sha256 {sha(dds[n])[:16]} is not the pinned {DDS_SHA256[n][:16]}')
    a = hot.parse(data)
    out, written, applied = bytearray(a.data), [], []
    for n in NAMES:
        hits = [e for e in a.entries if e.name.lower() == n]
        if len(hits) != 1:
            raise Refused(f'{area} {n}: {len(hits)} entries match by name (case-insensitive), expected 1')
        e, new, p = hits[0], dds[n], pins[n]
        if e.packed:
            raise Refused(f'{area} {e.name}: entry is packed')
        if e.hdr_size != HDR or e.raw != len(new) or e.raw != p['size']:
            raise Refused(f'{area} {e.name}: header {e.hdr_size} B / size {e.raw} B, expected {HDR} / {len(new)} '
                          f'(pin {p["size"]})')
        hdr, pix = a.header(e.name), a.stored(e.name)
        if hdr == new[:HDR] and pix == new[HDR:]:
            applied.append(e.name)
            continue
        bad = [k for k, ok in (('header', sha(hdr) == p['header_sha256']),
                               ('pixels', sha(pix) == p['pixels_sha256'])) if not ok]
        if bad:
            raise Refused(f'{area} {e.name}: {" and ".join(bad)} not the stock bytes (nor the new ones)')
        out[e.hdr_off:e.hdr_off + HDR] = new[:HDR]
        out[e.data_off:e.data_off + e.raw - HDR] = new[HDR:]
        written.append(e.name)
    if not written:
        return None, {'state': 'applied', 'area': area, 'entries': applied}
    return bytes(out), {'state': 'patched', 'area': area, 'entries': written, 'already': applied}
