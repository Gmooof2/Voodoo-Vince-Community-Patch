"""Pure firebug (roach shell, marker 8.0625) apply for the AIO (phase 1.6 T2): bytes in, bytes out, no file I/O.

apply_exe(exe, base): exe is always the composed Vince.exe (vpf_apply.COMPOSED); slots 34/36 get the roach branch
(build_roach_shell.shell_source) compiled ps_4_1 and validated by WARP, descriptor lengths updated.
- base 'vpf': on top of vpf_apply.apply(exe); the shell wraps the bead text vpf_apply builds.
- base 'stock': on the composed exe's stock slots; the shell wraps the uncorrected source. SpookyPS9 untouched.
The HLSL always comes from the composed exe's own SPDB: the VPF output's recompiled slots carry no debug chunk.
apply_models(name, data): marks the three roach gators of models.hot inside one of the three roach archives.
Siblings are loaded by path and registered in sys.modules (cookbook C0040); sys.path is left unchanged.

CLI: python -I firebug_apply.py [--exe build/aio/Vince.exe] --base vpf|stock --out <dir>
writes {34,36}-{baseline,patched}.{hlsl,dxbc} for test_roach_shell_gpu.py.
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def _load_vpf():
    path = ROOT / 'VoodooPowerFix' / 'vpf_apply.py'
    mod = sys.modules.get('vpf_apply')
    if mod is not None:
        if Path(getattr(mod, '__file__', '') or '').resolve() != path.resolve():
            raise ImportError(f'vpf_apply already loaded from elsewhere: {getattr(mod, "__file__", None)}')
        return mod
    spec = importlib.util.spec_from_file_location('vpf_apply', path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules['vpf_apply'] = mod
    try:
        spec.loader.exec_module(mod)
    except BaseException:
        del sys.modules['vpf_apply']
        raise
    return mod


vpf = _load_vpf()
bps, bbs = vpf.bps, vpf.bbs
hot = vpf._load('aio_hot', ROOT / 'aio' / 'hot.py')
# build_roach_shell edits sys.path at import for its own CLI; its siblings are already in sys.modules here.
_saved_path = list(sys.path)
try:
    brs = vpf._load('build_roach_shell', HERE / 'build_roach_shell.py')
finally:
    sys.path[:] = _saved_path
del _saved_path

BASES = ('vpf', 'stock')
# = components.json firebug touches: the archives holding the roach family in models.hot.
ARCHIVES = ('vincedata/levels/frenchquarter/area_mainstreet/modelsAndAnims.hot',
            'vincedata/levels/frenchquarter/area_thesquare/modelsAndAnims.hot',
            'vincedata/levels/sewers/common/modelsAndAnims.hot')


def _container(exe, index):
    """The DXBC container at slot `index`, cut to its header size (bounds-checked against the slot capacity)."""
    offset, capacity, _, _ = bps.SHADERS[index]
    slot = exe[offset:offset + capacity]
    if len(slot) != capacity or slot[:4] != b'DXBC':
        raise ValueError(f'Slot {index}: no DXBC at {offset:#x}')
    size, = struct.unpack_from('<I', slot, 24)
    if not 32 <= size <= capacity or any(slot[size:]):
        raise ValueError(f'Slot {index}: bad DXBC size {size}')
    return slot[:size]


def _build(exe, base):
    """-> (patched exe, {index: (baseline hlsl, baseline dxbc, patched hlsl, patched dxbc)})."""
    exe = bytes(exe)
    if base not in BASES:
        raise ValueError(f'firebug apply: unknown base {base!r}')
    if hashlib.sha256(exe).hexdigest() != vpf.COMPOSED:
        raise ValueError('firebug apply: input is not the composed exe ' + vpf.COMPOSED[:16])
    out = vpf.apply(exe) if base == 'vpf' else exe
    shaders = {}
    for index, (_, _, _, entry) in bps.SHADERS.items():
        texts = vpf.slot_texts(exe, index)
        if base == 'vpf':
            source = bbs.bead_source(bps.source_text(texts, index, corrected=True), index)
        else:
            source = bps.source_text(texts, index, corrected=False)
        baseline = _container(out, index)
        text = brs.shell_source(source, index, base)
        shell = vpf._compile(f'{index}-patched.hlsl', text, entry)
        if base == 'vpf':
            out, _ = brs.patch_slot(out, index, baseline, shell)
        else:
            out, _ = bps.patch_shader(out, index, shell)
        shaders[index] = (source, baseline, text, shell)
    return out, shaders


def apply_exe(exe: bytes, base: str) -> bytes:
    return _build(exe, base)[0]


def apply_models(name: str, data: bytes) -> bytes:
    """Mark the roach shell (power 0 -> 8.0625) in the three roach gators of the nested models.hot; every other
    entry of both archives keeps its payload. Refuses any archive but the three roach archives."""
    if name not in ARCHIVES:
        raise ValueError(f'firebug apply_models: not a roach archive: {name!r}')
    outer = hot.parse(data)
    inner = hot.parse(outer.payload('models.hot'))
    if not set(brs.MODELS) <= set(inner.by_name):
        raise ValueError(f'{name}: incomplete roach family')
    marked = {model: brs.mark_shell(inner.payload(model)) for model in brs.MODELS}
    new_inner = hot.repack(inner, marked)
    result = hot.repack(outer, {'models.hot': new_inner})
    check_outer, check_inner = hot.parse(result), hot.parse(new_inner)
    for archive, check, changed in ((outer, check_outer, {'models.hot': new_inner}), (inner, check_inner, marked)):
        if [e.name for e in check.entries] != [e.name for e in archive.entries]:
            raise AssertionError('Entry order changed')
        for e in archive.entries:
            if check.payload(e.name) != changed.get(e.name, archive.payload(e.name)):
                raise AssertionError(f'Payload mismatch after repack: {e.name}')
    return result


def dump_shaders(exe, base, out_dir):
    """Write {34,36}-{baseline,patched}.{hlsl,dxbc} into out_dir (the GPU harness layout)."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for index, (source, baseline, text, shell) in _build(exe, base)[1].items():
        for kind, hlsl, dxbc in (('baseline', source, baseline), ('patched', text, shell)):
            (out_dir / f'{index}-{kind}.hlsl').write_text(hlsl)
            (out_dir / f'{index}-{kind}.dxbc').write_bytes(dxbc)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--exe', type=Path, default=ROOT / 'build' / 'aio' / 'Vince.exe')
    parser.add_argument('--base', choices=BASES, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    dump_shaders(args.exe.read_bytes(), args.base, args.out)
    print(f'wrote {args.out}')
