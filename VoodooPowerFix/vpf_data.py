"""Pure VoodooPowerFix data apply for the AIO (phase 1.6 T3): one modelsAndAnims.hot in, the marked archive out.

apply_models(name, data) marks, in the nested models.hot of one of the 16 archives components.json voodoopowerfix
touches, the specular-power markers of the legacy builders (no file I/O):
- pickup: powericon_base 20.0 -> 20.125 (pickup_shading.mark_pickup), the 7 common archives;
- bead: bead/bead2/bead3_base 74.89 -> 74.875 (build_bead_shading.mark_bead), the 7 common archives;
- skull: the two power-skull gators each archive holds (interface* in the commons, world ones in the areas),
  20.0 -> 20.125 (build_skull_shading.mark_skull).
Each touched gator must be its stock bytes (STOCK sha256; one sha per name across all 16 archives, measured T3.c1),
so already-marked input is refused, not passed through. Parse/repack through aio/hot.py only: nested models.hot
repacked in its stock layout (= the legacy in-place splice), the outer archive with pad4 (= the legacy
pickup_shading.rebuild_compressed layout, so the outputs equal the legacy builds under the same zlib).
Siblings are loaded by path and registered in sys.modules (cookbook C0040); sys.path is never edited.
"""
import hashlib
import importlib.util
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


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


hot = _load('aio_hot', ROOT / 'aio' / 'hot.py')
_load('build_pickup_shaders', HERE / 'build_pickup_shaders.py')
_load('inspect_pickups', HERE / 'inspect_pickups.py')
ps = _load('pickup_shading', HERE / 'pickup_shading.py')
bbs = _load('build_bead_shading', HERE / 'build_bead_shading.py')
bss = _load('build_skull_shading', HERE / 'build_skull_shading.py')

PARTS = ('pickup', 'bead', 'skull')
# = components.json voodoopowerfix touches (*.hot): 7 commons (pickup, bead, interface skulls), 9 areas (world skulls).
COMMON = tuple(f'vincedata/levels/{lv}/modelsAndAnims.hot' for lv in (
    'bayou/common', 'carnival/common', 'cryptcity/common', 'frenchquarter/common', 'manorhouse/common',
    'sewers/common', 'voodooshop/area_voodooshop'))
AREAS = tuple(f'vincedata/levels/{lv}/modelsAndAnims.hot' for lv in (
    'bayou/area_crawdadjimmys', 'bayou/area_earthwoodandwater', 'carnival/area_landorides',
    'cryptcity/area_gwistlethorpemausoleum', 'cryptcity/area_sarcophagushustle', 'frenchquarter/area_mainstreet',
    'frenchquarter/area_thesquare', 'manorhouse/area_glowberrytangle', 'sewers/area_stealthroom'))
ARCHIVES = tuple(sorted(COMMON + AREAS))
# Stock gator sha256 (identical in every archive that holds the gator; stock oracle, T3.c1).
STOCK = {
    'powericon_base.gator': '731ca3d86ddbaf6d9360a9de7e397e3059b4610a20489a99e3f082f93614b0fe',
    'bead_base.gator': '45b44de3fe8113e7bd84d991b6ad85a754e1d078d35b223b40a4db9673eb852e',
    'bead2_base.gator': '8559a3d35bae15e62e375336a4e91f20204e534283c2b5d8f7dcf2e40042955a',
    'bead3_base.gator': '68f8503b54e31f7e63eb99eee13bf8202d34df42255233cc80a35ea437a02fcc',
    'mysterybox_base.gator': 'ee0d26ca1f6182e32d5c578bc595871ffb5c60d34ccc999270bad6dc8e61fbc5',
    'goldenjunebug_base.gator': 'ee0d26ca1f6182e32d5c578bc595871ffb5c60d34ccc999270bad6dc8e61fbc5',
    'interfacemysterybox_base.gator': 'a408b68c3ccc3a49e71dfbfb53d20711bc8cb9bb6fd012b6efda0e517b72de6d',
    'interfacegoldenjunebug_base.gator': 'a408b68c3ccc3a49e71dfbfb53d20711bc8cb9bb6fd012b6efda0e517b72de6d',
}


def gators(name, parts=PARTS):
    """{gator: marker function} the given parts touch in archive `name`."""
    if name not in ARCHIVES:
        raise ValueError(f'vpf apply_models: not a VoodooPowerFix archive: {name!r}')
    if not set(parts) <= set(PARTS):
        raise ValueError(f'vpf apply_models: unknown parts {parts!r}')
    out = {}
    if name in COMMON and 'pickup' in parts:
        out['powericon_base.gator'] = lambda m, n: ps.mark_pickup(m)
    if name in COMMON and 'bead' in parts:
        out.update({g: bbs.mark_bead for g in bbs.MODELS})
    if 'skull' in parts:
        skulls = [g for g in bss.MODELS if g.startswith('interface') == (name in COMMON)]
        out.update({g: bss.mark_skull for g in skulls})
    return out


def _apply(name, data, parts):
    outer = hot.parse(data)
    inner = hot.parse(outer.payload('models.hot'))
    marked = {}
    for gator, mark in gators(name, parts).items():
        if gator not in inner.by_name:
            raise ValueError(f'{name}: missing {gator}')
        model = inner.payload(gator)
        if hashlib.sha256(model).hexdigest() != STOCK[gator]:
            raise ValueError(f'{name}: {gator} is not its stock bytes')
        marked[gator] = mark(model, gator)
        if marked[gator] == model:
            raise AssertionError(f'{name}: {gator} unchanged by its marker')
    new_inner = hot.repack(inner, marked)
    result = hot.repack(outer, {'models.hot': new_inner}, pad4=True)
    check_outer, check_inner = hot.parse(result), hot.parse(new_inner)
    for archive, check, changed in ((outer, check_outer, {'models.hot': new_inner}), (inner, check_inner, marked)):
        if [e.name for e in check.entries] != [e.name for e in archive.entries]:
            raise AssertionError('Entry order changed')
        for e in archive.entries:
            if check.payload(e.name) != changed.get(e.name, archive.payload(e.name)):
                raise AssertionError(f'Payload mismatch after repack: {e.name}')
    return result


def apply_models(name: str, data: bytes) -> bytes:
    """Mark every VoodooPowerFix gator (pickup, bead, skull) of archive `name`; refuses unknown archives and any
    touched gator that is not its stock bytes."""
    return _apply(name, bytes(data), PARTS)
