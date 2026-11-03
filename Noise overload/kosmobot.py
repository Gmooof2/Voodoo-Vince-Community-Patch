"""Noise overload, on-disk half: Inside Kosmobot sounds.hot with the three loud kik loops at prototype level.

Pure aio/build.py data-chain step: patch_sounds_hot(data) -> (bytes, report). Stdlib only, writes no files.
Scales the three retail PCM payloads in place (same size, no repack); every other byte is kept.
Refuses unless each payload is the Steam retail sample (RETAIL_SHA256) and asserts each result (PATCHED_SHA256).
"""
import array
import hashlib
import importlib.util
import sys
from pathlib import Path

BANK = 'vincedata/levels/carnival/area_insidekosmobot/sounds.hot'

# the Steam release's samples (copied from vince_volume_ab.py:16-20)
RETAIL_SHA256 = {
    'kik_objectswing.wav': '0ac78eaedcad99baf1f7a30f93c5bc2960c3b8d973a3e6975922903f521023ac',
    'kik_platformhover.wav': 'a5f3daf761cdf03513ec32bd0b1f1ec397df17245b7e5197859047166b9127a9',
    'kik_rotatingplatform.wav': 'c8000b02c0c74236a45ce07b4bf5f369117715339c330a4525a2cfcecf51ae8d',
}

# dB from retail to prototype, measured RMS (vince_volume_ab.py:23-27)
PROTOTYPE_DB = {
    'kik_platformhover.wav': -12.0,  # was -18.0 (1.8 T11, +6 dB)
    'kik_rotatingplatform.wav': -12.0,  # was -18.0 (1.8 T11, +6 dB)
    'kik_objectswing.wav': -6.2,  # was -12.2 (1.8 T11, +6 dB)
}

# sha256 of each scaled payload, computed once from the stock oracle bank 19eb2e68 (1.8 T11; was 1.5 T3.c1)
PATCHED_SHA256 = {
    'kik_platformhover.wav': '6037e375450579cfb3bfee2544d192749671a4a8bf151a12f92284bfd1192016',
    'kik_rotatingplatform.wav': '3e08f39bc3ee8f554d043a49c5d371d7b4aceb88dd1dfeb7546cea43b3ad2f94',
    'kik_objectswing.wav': '699d5146b139c7fd531ab6eccf49342a2f61ae5b4f39a7e98c86751198c5969d',
}


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


hot = _load('aio_hot', Path(__file__).resolve().parent.parent / 'aio' / 'hot.py')


class Refused(hot.Refused):
    """Not the retail Inside Kosmobot bank."""


def scale(pcm, db):
    g = 10 ** (db / 20)
    a = array.array('h')
    a.frombytes(pcm)
    if sys.byteorder != 'little':
        a.byteswap()
    for i, x in enumerate(a):
        a[i] = max(-32768, min(32767, round(x * g)))
    if sys.byteorder != 'little':
        a.byteswap()
    return a.tobytes()


def patch_sounds_hot(data):
    table = hot.bank(hot.parse(data))
    out = bytearray(data)
    report = {}
    for name, db in PROTOTYPE_DB.items():
        if name not in table:
            raise Refused(f'{name}: not in the bank')
        off, size = table[name]
        pcm = bytes(data[off:off + size])
        if hashlib.sha256(pcm).hexdigest() != RETAIL_SHA256[name]:
            raise Refused(f'{name}: not the retail sample')
        new = scale(pcm, db)
        assert len(new) == size
        got = hashlib.sha256(new).hexdigest()
        if got != PATCHED_SHA256.get(name):
            raise Refused(f'{name}: scaled sha256 {got[:16]} is not the pinned one')
        out[off:off + size] = new
        report[name] = db
    return bytes(out), report
