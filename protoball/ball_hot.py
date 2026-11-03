"""protoball, AIO builder: the ball's mesh hidden and its particles toned (Zombie Guidance Counselor).

Pure aio/build.py data-chain steps, stdlib only, write no files:
- hide_ball_mesh(data) -> (bytes, report): modelsAndAnims.hot, models.hot/gbcml_base.gator strip indices zeroed
  (protoball.zip:protoball.py:hide_ball_mesh :105-119).
- tone_particles(data, purple, cyan, spark_slot) -> (bytes, report): particles.hot, gbcml.fly spawn alpha scaled and
  the cyan emitter's 4th texture slot (protoball.py:tone_emitters :145-152, fix_spark_slot :155-180).
protoball.py's layout checks kept; archives parsed and repacked by aio/hot.py (nested models.hot repacked inside the
outer archive; gbcml.fly is stored raw, hdr_size 0, so its repack keeps the size protoball.py edits in place).
Refuses unless the stock entry is the pinned one; asserts the result entry (pins reproduced from the oracle, 1.5 T9).
"""
import hashlib
import importlib.util
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
MODELS = 'vincedata/levels/cryptcity/area_zombieguidancecounselor/modelsAndAnims.hot'
PARTICLES = 'vincedata/levels/cryptcity/common/particles.hot'
INNER = 'models.hot'
GATOR = 'gbcml_base.gator'
FLY = 'gbcml.fly'

STOCK_GATOR_SHA256 = 'a9cfe0a0c2ee376a6254e4e2ab44d6d41cb13c88efbea5a6ef94502df35699a4'
STOCK_FLY_SHA256 = 'b1b19fb2d7a604ae7bbc9536edff84d713d6bd1b8878e56210f010fed5b9e4a9'
HIDDEN_GATOR_SHA256 = '002b6c4ac7cb70dd83c151ab93c67a7c1eab4c8242e8825dcca378001faff2d8'
# --purple 0.15 --cyan 0.6 --spark-slot (protoball/README.md current setting)
TONED_FLY_SHA256 = '224033ca0a849c509823916180c4b37ed6296a7b192d9975ca73066473286fc7'


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


hot = _load('aio_hot', HERE.parent / 'aio' / 'hot.py')


class Refused(hot.Refused):
    """Not the stock ball model or particle file, or not the layout protoball.py was written for."""


def sha(b):
    return hashlib.sha256(b).hexdigest()


def _stock(archive, name, pinned):
    b = archive.payload(name)
    if sha(b) != pinned:
        raise Refused(f'{name}: not the stock entry (sha256 {sha(b)[:16]}, pinned {pinned[:16]})')
    return b


def _result(name, b, pinned):
    got = sha(b)
    if got != pinned:
        raise Refused(f'{name}: result sha256 {got[:16]} is not the pinned {pinned[:16]}')
    return b


def hide_ball_mesh(data):
    outer = hot.parse(data)
    inner = hot.parse(outer.payload(INNER))
    g = bytearray(_stock(inner, GATOR, STOCK_GATOR_SHA256))
    if g[:4] != b'GATR':
        raise Refused(f'{GATOR} is not a GATR model')
    num_idx, num_strips = struct.unpack_from('<II', g, 0x14)   # GatorFile.numIndices, numStrips
    off_idx = struct.unpack_from('<I', g, 0x34)[0]
    if (num_idx, num_strips) != (427, 1) or off_idx + 2 * num_idx > len(g):
        raise Refused(f'{GATOR} layout differs from the one this patch was written for')
    struct.pack_into('<%dH' % num_idx, g, off_idx, *([0] * num_idx))
    new = _result(GATOR, bytes(g), HIDDEN_GATOR_SHA256)
    return hot.repack(outer, {INNER: hot.repack(inner, {GATOR: new})}), {'indices_zeroed': num_idx}


def is_purple(max_particles, render_color):
    return max_particles == 200 and render_color[0] > 0.3 and render_color[2] > 0.5


def is_cyan(max_particles, render_color):
    return max_particles == 20 and render_color[1] > 0.2 and render_color[2] > 0.25


def _emitters(fly):
    if fly[:4] != b'FFLY':
        raise Refused(f'{FLY} is not an FFLY file')
    num_emitters, off_emitters = struct.unpack_from('<I', fly, 0x18)[0], struct.unpack_from('<I', fly, 0x24)[0]
    if off_emitters + num_emitters * 0x180 > len(fly):
        raise Refused(f'{FLY}: emitter table out of range')
    return [off_emitters + i * 0x180 for i in range(num_emitters)]


def _matches(fly, em, match):
    data = em + 0x34                                    # FFlyBinEmitter.data (FFlyData)
    return match(struct.unpack_from('<I', fly, data + 0x70)[0], struct.unpack_from('<3f', fly, data + 0xf4))


def tone_emitters(fly, purple, cyan):
    """protoball.py:tone_emitters on the gbcml.fly payload: FFlyData._alpha (min, max) scaled."""
    for match, factor in ((is_purple, purple), (is_cyan, cyan)):
        if factor != 1:
            em = next((em for em in _emitters(fly) if _matches(fly, em, match)), None)
            if em is None:
                raise Refused('%s emitter not found in %s' % (match.__name__[3:], FLY))
            pos = em + 0x34 + 0xb0
            lo, hi = struct.unpack_from('<2f', fly, pos)
            struct.pack_into('<2f', fly, pos, lo * factor, hi * factor)


def fix_spark_slot(fly):
    """protoball.py:fix_spark_slot on the gbcml.fly payload: the cyan emitter gets a 4th texture (ball_001)."""
    emitters = _emitters(fly)
    tex_lists = [struct.unpack_from('<I', fly, em + 0x14)[0] for em in emitters]   # FFlyBinEmitter.offsetTextures
    for em, tex_list in zip(emitters, tex_lists):
        if not _matches(fly, em, is_cyan):
            continue
        num_tex = struct.unpack_from('<I', fly, em + 0x8)[0]
        render_tex = struct.unpack_from('<f', fly, em + 0x34 + 0x114)[0]
        free_slot = tex_list + num_tex * 4
        if (num_tex, render_tex) != (3, 3.0) or free_slot + 4 > len(fly) \
                or struct.unpack_from('<I', fly, free_slot)[0] != 0 \
                or any(free_slot < t < free_slot + 4 or t == free_slot for t in tex_lists if t != tex_list):
            raise Refused('cyan emitter layout differs from the one this fix was written for')
        struct.pack_into('<I', fly, em + 0x8, 4)        # numTextures 3 -> 4
        struct.pack_into('<I', fly, free_slot, 0)       # 4th texture = string 0 (ball_001.dds)
        return
    raise Refused(f'cyan emitter not found in {FLY}')


def tone_particles(data, purple, cyan, spark_slot):
    outer = hot.parse(data)
    fly = bytearray(_stock(outer, FLY, STOCK_FLY_SHA256))
    tone_emitters(fly, purple, cyan)
    if spark_slot:
        fix_spark_slot(fly)
    new = _result(FLY, bytes(fly), TONED_FLY_SHA256)
    return hot.repack(outer, {FLY: new}), {'purple': purple, 'cyan': cyan, 'spark_slot': spark_slot}
