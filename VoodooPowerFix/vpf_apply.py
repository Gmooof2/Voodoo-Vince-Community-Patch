"""Pure VoodooPowerFix exe apply for the AIO (phase 1.6 T1): composed Vince.exe bytes in, patched bytes out.

apply(exe) rebuilds, with the existing builders and no file I/O:
- slots 34 OptimizedPhongPS / 36 PhongPS: pickup correction (build_pickup_shaders, always-on) then the bead branch
  (build_bead_shading.bead_source), from the HLSL in the input slot's own SPDB chunk;
- SpookyPS9 at 0x385308: halo mask blur (build_pickup_halo.PATCHED);
each compiled ps_4_1 and validated by WARP, descriptor lengths updated. Refuses any input but COMPOSED.
SpookyVS at 0x3903F8 belongs to the spooky record and is never written here.
Siblings are loaded by path and registered in sys.modules (cookbook C0040); sys.path is never edited.
"""
import hashlib
import importlib.util
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent / 'VinceWaterFix' / 'tools'
# = build/aio/ownership.json final_sha256 (phase 1.6 composed exe).
COMPOSED = '9a21196576f38d557eeac7dc7a5fb7cb8141aa140cc939ccc2b3e8799aa6edcc'  # was 6f4417a8… (1.7 T14), 1.8 T10 beam payload
PROFILE = 'ps_4_1'
SOURCE_RUN = re.compile(rb'[\x09\x0a\x0d\x20-\x7e]{160,}')  # = extract_remaster_shaders.py source-stream regex


def _load(name, path):
    """Load a sibling module by path under its bare name; reuse it only when it is the same file."""
    mod = sys.modules.get(name)
    if mod is not None:
        if Path(getattr(mod, '__file__', '') or '').resolve() != path.resolve():
            raise ImportError(f'{name} already loaded from elsewhere: {getattr(mod, "__file__", None)}')
        return mod
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    try:
        spec.loader.exec_module(mod)
    except BaseException:
        del sys.modules[name]
        raise
    return mod


d3d = _load('d3d_tools', TOOLS / 'd3d_tools.py')
ers = _load('extract_remaster_shaders', TOOLS / 'extract_remaster_shaders.py')
bps = _load('build_pickup_shaders', HERE / 'build_pickup_shaders.py')
_load('stage_pickup_fov', HERE / 'stage_pickup_fov.py')
bph = _load('build_pickup_halo', HERE / 'build_pickup_halo.py')
_load('inspect_pickups', HERE / 'inspect_pickups.py')
_load('pickup_shading', HERE / 'pickup_shading.py')
bbs = _load('build_bead_shading', HERE / 'build_bead_shading.py')

# name -> (file offset, capacity); the bytes apply() owns besides the three descriptor length dwords.
RANGES = {'slot34': bps.SHADERS[34][:2], 'slot36': bps.SHADERS[36][:2], 'SpookyPS9': (bph.OFFSET, bph.CAPACITY)}


class _Source:
    """In-memory stand-in for the Path compile_shader takes: it calls only read_bytes() and str().
    Bytes are what the builder scripts' Path.write_text() puts on disk on Windows (CRLF, ASCII)."""

    def __init__(self, name, text):
        self.name = name
        self.data = text.replace('\n', '\r\n').encode('ascii')

    def read_bytes(self):
        return self.data

    def __str__(self):
        return self.name


def _compile(name, text, entry):
    bytecode = d3d.compile_shader(_Source(name, text), entry, PROFILE)
    d3d.validate_pixel_shader(bytecode)
    return bytecode


def slot_texts(exe, index):
    """{PDB stream: HLSL text} of slot `index`, as extract_remaster_shaders writes and source_for reads it back."""
    offset, capacity, _, _ = bps.SHADERS[index]
    found = next(ers.containers(exe[offset:offset + capacity]), None)
    if found is None or found[0] != 0 or 'SPDB' not in found[2]:
        raise ValueError(f'Slot {index}: no DXBC with SPDB at {offset:#x}')
    streams = ers.pdb_streams(found[2]['SPDB'])
    texts = {}
    for stream in bps.STREAMS:
        match = SOURCE_RUN.match(streams[stream]) if stream < len(streams) else None
        if match is None:
            raise ValueError(f'Slot {index}: PDB stream {stream} has no source text')
        # extractor: decode + CRLF->LF, write newline='\n'; source_for: read_text() (universal newlines).
        texts[stream] = match.group().decode('ascii').replace('\r\n', '\n').replace('\r', '\n')
    return texts


def apply(exe: bytes) -> bytes:
    exe = bytes(exe)
    if hashlib.sha256(exe).hexdigest() != COMPOSED:
        raise ValueError('VoodooPowerFix apply: input is not the composed exe ' + COMPOSED[:16])
    out = exe
    for index, (_, _, _, entry) in bps.SHADERS.items():
        pickup = bps.source_text(slot_texts(exe, index), index, corrected=True, comparison=False)
        prior = _compile(f'{index}-patched.hlsl', pickup, entry)
        out, _ = bps.patch_shader(out, index, prior)
        bead = _compile(f'{index}-bead.hlsl', bbs.bead_source(pickup, index), entry)
        out, _ = bbs.patch_slot(out, index, prior, bead)
    halo = _compile('patched.hlsl', bph.PATCHED, 'SpookyPS')
    out, _ = bph.patch_executable(out, halo, previous=None)
    return out
