"""VinceJanicePupilFix, AIO builder: Vince's Xbox pupil curves restored in the Janice intro cutscene.

Pure aio/build.py data-chain step: patch_models_hot(data) -> (bytes, report). Stdlib only, writes no files.
make_pupilfix.py's steps (patch_anim :83-120, the archive repack :134-141) with the curves read from the committed
pupil_curves.json instead of the Xbox archive, and the archives parsed and repacked by aio/hot.py (no zlib-level
assert, S-pupilfix-03). Only vince_janiceintro001.anim inside anims.hot changes; models.hot and the other anims keep
their stored bytes. Refuses unless the stock .anim and the curves file are the pinned ones; asserts the result .anim
(S-pupilfix-01).
"""
import hashlib
import importlib.util
import json
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
ARCHIVE = 'vincedata/levels/sewers/area_wrathofjanice/modelsAndAnims.hot'
INNER = 'anims.hot'
ANIM = 'vince_janiceintro001.anim'
PUPIL = 25                      # vince_base.gator bone index
POS_CHANNELS = (1, 2, 3)        # px, py, pz

CURVES_SHA256 = '3a68f2f9981b6cdb700015506c51821fc8089d692b229b03e7892a0d84c25035'
STOCK_ANIM_SHA256 = 'e665c8b21548d26ff63eddd44a083f8511ddb0032b2f879234c2151cb41acb4c'
# the download's fixed .anim (area_wrathofjanice_pupilfix.zip a6f0c952..., README.md:14)
FIXED_ANIM_SHA256 = '51766d2873a23ce84e9d0684e6f01a25c2b0029510a37e26e3062e10f67e0a77'


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


hot = _load('aio_hot', HERE.parent / 'aio' / 'hot.py')


class Refused(hot.Refused):
    """Not the stock Wrath of Janice archive, or not the pinned curves."""


def sha(b):
    return hashlib.sha256(b).hexdigest()


def al(x, a):
    return (x + a - 1) & ~(a - 1)


def load_curves(raw=None):
    """{channel: floats as stored (5n+1)} from pupil_curves.json, sha-pinned."""
    raw = (HERE / 'pupil_curves.json').read_bytes() if raw is None else raw
    if sha(raw) != CURVES_SHA256:
        raise Refused('pupil_curves.json is not the pinned 3a68f2f9')
    j = json.loads(raw)
    if j['anim'] != ANIM or j['bone'] != PUPIL:
        raise Refused('pupil_curves.json: wrong anim or bone')
    out = {}
    for k in POS_CHANNELS:
        c = j['channels'][str(k)]
        f = [float(x) for x in c['floats']]
        n = c['n']
        if not n or len(f) != 5 * n + 1:
            raise Refused(f'channel {k}: {len(f)} floats for {n} segments')
        out[k] = (n, f)
    return out


def patch_anim(rem, curves):
    """make_pupilfix.py:patch_anim with the Xbox curves given (their -1/3 s start already moved to 0)."""
    o_tr_r = struct.unpack_from('<I', rem, 0x1C)[0]
    if o_tr_r + 128 * (PUPIL + 1) > len(rem):
        raise Refused('track table out of range')
    blob, new = bytearray(), {}
    for k in POS_CHANNELS:
        n, f = curves[k]
        new[k] = (n, len(blob))
        blob += struct.pack('<%df' % len(f), *f)
    blob += b'\0' * (al(len(blob), 16) - len(blob))
    ins, shift = o_tr_r, len(blob)          # insert just before the track table

    out = bytearray(rem[:ins]) + blob + bytearray(rem[ins:])
    fix = lambda v: v + shift if v >= ins else v
    # header: tracks, events, (unused), event names, strings
    for off in (0x1C, 0x20, 0x24, 0x28, 0x2C):
        v = struct.unpack_from('<I', out, off)[0]
        if v:
            struct.pack_into('<I', out, off, fix(v))
    o_tr, o_ev = struct.unpack_from('<2I', out, 0x1C)
    n_tr, n_ev = struct.unpack_from('<I', out, 0x0C)[0], struct.unpack_from('<I', out, 0x10)[0]
    if o_tr + 128 * n_tr > len(out) or o_ev + 16 * n_ev > len(out):
        raise Refused('track or event table out of range')
    for i in range(n_tr):
        for k in range(10):
            p = o_tr + 128 * i + 0x50 + 4 * k
            struct.pack_into('<I', out, p, fix(struct.unpack_from('<I', out, p)[0]))
    for i in range(n_ev):
        p = o_ev + 16 * i
        struct.pack_into('<I', out, p, fix(struct.unpack_from('<I', out, p)[0]))
    # point the pupil's position channels at the inserted curves
    t = o_tr + 128 * PUPIL
    for k, (n, rel) in new.items():
        struct.pack_into('<I', out, t + 8 * k, n)
        struct.pack_into('<I', out, t + 0x50 + 4 * k, ins + rel)
    return bytes(out)


def patch_models_hot(data, curves_raw=None):
    outer = hot.parse(data)
    inner_bytes = outer.payload(INNER)
    inner = hot.parse(inner_bytes)
    stock = inner.payload(ANIM)
    if sha(stock) != STOCK_ANIM_SHA256:
        raise Refused(f'{ANIM}: not the stock animation')
    fixed = patch_anim(stock, load_curves(curves_raw))
    got = sha(fixed)
    if got != FIXED_ANIM_SHA256:
        raise Refused(f'{ANIM}: result sha256 {got[:16]} is not the pinned {FIXED_ANIM_SHA256[:16]}')
    new_inner = hot.repack(inner, {ANIM: fixed})
    return hot.repack(outer, {INNER: new_inner}), {'anim': f'{len(stock)} -> {len(fixed)} B'}
