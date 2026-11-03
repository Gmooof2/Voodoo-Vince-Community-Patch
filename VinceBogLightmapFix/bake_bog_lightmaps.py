"""Bake world lightmaps into BOG (sorted alpha geometry) vertex colours.

Voodoo Vince Remastered draws BOG triangles (palm fronds, bushes, tree cards)
with BogPS, which outputs vertexColour * baseTexture and never samples the
material's lightmap. Most of those vertex colours are black, so lightmapped
foliage renders black. World geometry instead uses WorldPS:

    saturate((vertexColour + saturate(lightmap + dynamic)) * base)

This tool writes clamp(vertexColour + lightmap(frac(TEXCOORD2 * 16)), 0, 1)
into the vertex colour of every vertex used by a lightmapped BOG triangle, so
the unchanged BogPS gives the world-shader result (minus dynamic lights).
Vertices of non-lightmapped materials are left untouched.

Library use (AIO build pipelines): patch_world_hot(world, textures, area)
takes the current world.hot and textures.hot bytes and returns new world.hot
bytes, or None when the area is already baked or needs nothing. Only BOG
vertex colours change; every other archive entry is carried over unchanged.

Commands:
  status  <game>               report each area: retail, baked, or unknown
  stage   <game> <stage_dir>   read the game, write baked world.hot files + manifest
  install <game> <stage_dir>   copy staged files in, backing up the originals
  restore <game> <stage_dir>   put the backed-up originals back

Guards: a BOG entry is only baked when its bytes match the pinned retail
Steam 1.14.2.0 hash, and is recognised as done when it matches the pinned
baked hash; anything else is refused. Other entries of world.hot (index.tdf,
collision, vertices) may have been changed by other mods. Install and restore
additionally check whole-file hashes so they never overwrite unexpected files.
"""
import argparse
import hashlib
import json
import math
import re
import shutil
import struct
import zlib
from pathlib import Path

# area: (BOG entry in verts.hot, retail SHA-256, baked SHA-256 from retail lightmaps)
BOG_HASHES = {
    'bayou/area_belowdecks': ('zone0003.bog', '02e84765a12c78537f9c419f41de11e95552ce9f97d1a04c4b8b045303f6f6b3',
        '5939cbecc1c9205deb2863daf5f0bca28b8e1c139e3d9d3bb93055f2b95e5025'),
    'bayou/area_crawdadjimmys': ('zone0001.bog', '629ca3a157a65caa41fe71a2e967919b9f014ce522e6222b56eb076e0c8e5f80',
        '8a4736f837aaaa1d0ac0d914d7492117a7d481e2237f77196e24c3f7aa8710f5'),
    'bayou/area_earthwoodandwater': ('zone0004.bog', 'e1a2fd77593a8d2d897319478e7b90a8855511b3f825564832685bc25df3e2bf',
        '89071ad6893fb7ee8e10bbf3a173cd97b4594c12f69bd8a5b2b41ab1a9a5c34a'),
    'bayou/area_hurricanehannah': ('zone0008.bog', '1c7dde1a984c0aaa854624f538a206b5ebed1879afac87814f7dba6799a2dbd3',
        '7086037b9ffda83d0ca84284bcf958efc2da755c7fa0afee1472e74c85ffe6a5'),
    'bayou/area_thebogwallow': ('zone0006.bog', '17dd3a0914bfe0a085a87687ad02ba2eb6cacb0116e2dd9a224693803840dc1f',
        '86251add99ffd5d5e605d19315953062fb38602cd6cee65812a03f1759fc7d5c'),
    'carnival/area_landorides': ('zone0002.bog', '2776739c12626223221281d5a88cea55ff2b7b21fa06243ba9046a34a0e61251',
        '64ff15ff4a57fd5c626d26a8cd9aba6ff05c1998f7e80439c8ca5bc982dc9ac0'),
    'cryptcity/area_gatekeeperjam': ('zone0001.bog', '4dc2169f9df4c2147e0092bafc0df1f7b80b820f6f0b0f2fdb0c0d39756af518',
        'b0c893010504ddf558a1cf3c4982aa60286862b4d774c67d20dae60bc203b3ea'),
    'cryptcity/area_gwistlethorpemausoleum': ('zone0004.bog', '2307a221f678d493ef2f98101c6c83aded6116531e50704499303168cbf68f02',
        '8b1035f7a0d62eb2d3b0169020fcc85015850725caf257d444403ee5851c54bd'),
    'cryptcity/area_sarcophagushustle': ('zone0002.bog', '8d51c580032bff149ea609a15a993558ebb470407b256f4d8d26cf91550f32a6',
        '4999b7fd20fd5bf11eaab113da7b305487c96e05dc46c9081d707213de9593c1'),
    'cryptcity/area_zombieguidancecounselor': ('zone0003.bog', 'b0a42f3b45b3cbdea6fbc5277116d6bb6cd7b9be1758a5781c00e3203970525a',
        '790913ed181ec4d26906ad8b655156c7d9ab8f2af17153110317c690f42c44b2'),
    'frenchquarter/area_bigpig': ('zone0003.bog', '045395956cfd75e7934897ea1c1f2a92c661c75bdb4e632819ba8d11367074fb',
        '1c8205b7545e53792c74e1391eef5df587b8860bdd778bf2286701f75ad575a5'),
    'frenchquarter/area_courtyard': ('zone0001.bog', '6023f84c107e4a85a79dc0716adc8b6f077c3ece0107228907489d6e211783fa',
        'dfbec2d1d5a539affe34ce2ea0ee88e00e89e7e01802daebd786300cd5252c67'),
    'manorhouse/area_frontporch': ('zone0001.bog', 'aa5587ae3f71994c71d41a48f268a4db1f936901fb885efff630847e37ce716f',
        'cc89a2a0905a7950f9bad0360bd5ceed718f74c38fc4d1c3a8281c7dadd6f3fd'),
    'manorhouse/area_playroom': ('zone0006.bog', 'b479650a21e2fb8aa0c12cea8c445519255efa8c82c38cbc10ff70059c325944',
        'af57a77308fc7b1ec18180c9720558ff98f538d03950c169b3b53367505297d6'),
}

BOG_VERTEX = 64          # loc, normal, color float3; tex0/1/2 float2; uint
COLOR_OFFSET = 24
TEX2_OFFSET = 52
# Stored UVs are the Xbox s16/32768 values. Lightmap UV = frac(tex2 * 16):
# with this mapping every world and BOG lightmap sample lands on a used
# lightmap texel (alpha != 0); plain *16 with a border misses ~20%.
LIGHTMAP_UV_SCALE = 16.0


def sha(data):
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------- HOT archives

def hot_entries(data):
    """PC HOT layout: 28-byte header, 32-byte entries, name pool, optional zlib."""
    if data[:4] != b'HOT ':
        raise ValueError('Not a HOT archive')
    version, _, _, size, names, count = struct.unpack_from('<6I', data, 4)
    if version != 1 or size != len(data) or names != 28 + 32 * count:
        raise ValueError('Unsupported HOT layout')
    out = {}
    for i in range(count):
        pos = 28 + 32 * i
        name_off, hs, ho, raw, packed, off = struct.unpack_from('<Q4IQ', data, pos)
        end = data.index(b'\0', names + name_off)
        name = data[names + name_off:end].decode('ascii')
        out[name] = dict(pos=pos, hs=hs, ho=ho, raw=raw, packed=packed, off=off)
    return out


def read_entry(data, e):
    """Entry payload; textures keep their DDS header in a separate block."""
    blob = data[e['off']:e['off'] + (e['packed'] or e['raw'] - e['hs'])]
    blob = data[e['ho']:e['ho'] + e['hs']] + (zlib.decompress(blob) if e['packed'] else blob)
    if len(blob) != e['raw']:
        raise ValueError('Unexpected entry size')
    return blob


def rebuild(data, replace):
    """Rewrite an archive with some entries replaced, keeping each entry's compression."""
    es = hot_entries(data)
    if any(e['hs'] for e in es.values()):
        raise ValueError('Rewriting archives with separate entry headers is not supported')
    start = min(e['off'] for e in es.values())
    out = bytearray(data[:start])
    for name, e in sorted(es.items(), key=lambda kv: kv[1]['off']):
        if name in replace:
            new = replace[name]
            blob = zlib.compress(new, 9) if e['packed'] else new
            struct.pack_into('<II', out, e['pos'] + 16, len(new), len(blob) if e['packed'] else 0)
        else:
            blob = data[e['off']:e['off'] + (e['packed'] or e['raw'])]
        out += b'\0' * (-len(out) % 4)
        struct.pack_into('<Q', out, e['pos'] + 24, len(out))
        out += blob
    out += b'\0' * (-len(out) % 4)
    struct.pack_into('<I', out, 16, len(out))
    rebuilt = hot_entries(bytes(out))
    for name, e in es.items():
        expected = replace.get(name) or read_entry(data, e)
        if read_entry(out, rebuilt[name]) != expected:
            raise ValueError(f'Archive round trip failed: {name}')
    return bytes(out)


# ---------------------------------------------------------------- textures

def c565(c):
    return (((c >> 11) & 31) / 31, ((c >> 5) & 63) / 63, (c & 31) / 31)


def decode_dds_rgb(d):
    """Decode mip 0 of a DXT1/DXT5 DDS to a list of RGB float triples."""
    if d[:4] != b'DDS ':
        raise ValueError('Not a DDS')
    h, w = struct.unpack_from('<II', d, 12)
    fourcc = d[84:88]
    if fourcc not in (b'DXT1', b'DXT5'):
        raise ValueError(f'Unsupported lightmap format {fourcc!r}')
    px = [None] * (w * h)
    p = 128
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            if fourcc == b'DXT5':
                p += 8
            c0, c1, idx = struct.unpack_from('<HHI', d, p)
            p += 8
            a, b = c565(c0), c565(c1)
            if c0 > c1 or fourcc == b'DXT5':
                cols = [a, b, tuple((2 * x + y) / 3 for x, y in zip(a, b)), tuple((x + 2 * y) / 3 for x, y in zip(a, b))]
            else:
                cols = [a, b, tuple((x + y) / 2 for x, y in zip(a, b)), (0.0, 0.0, 0.0)]
            for i in range(16):
                x, y = bx + i % 4, by + i // 4
                if x < w and y < h:
                    px[y * w + x] = cols[(idx >> (2 * i)) & 3]
    return w, h, px


def sample_bilinear(tex, u, v):
    """Linear filtering at mip 0 with wrap addressing."""
    w, h, px = tex
    x, y = u * w - 0.5, v * h - 0.5
    x0, y0 = math.floor(x), math.floor(y)
    fx, fy = x - x0, y - y0
    out = [0.0, 0.0, 0.0]
    for dx, dy, wt in ((0, 0, (1 - fx) * (1 - fy)), (1, 0, fx * (1 - fy)), (0, 1, (1 - fx) * fy), (1, 1, fx * fy)):
        c = px[((y0 + dy) % h) * w + (x0 + dx) % w]
        for k in range(3):
            out[k] += c[k] * wt
    return out


# ---------------------------------------------------------------- world data

def lightmapped_materials(tdf):
    text = tdf.decode('latin1')
    if '[Materials]' not in text:
        return {}
    text = text[text.index('[Materials]'):]
    out = {}
    for m in re.finditer(r'\[(\d+)\]\s*\{([^}]*)\}', text):
        lm = re.search(r'lightmap\s*=\s*([^;]+);', m[2])
        if lm:
            out[int(m[1])] = lm[1].strip().lower()
    return out


def bogs(data):
    """Yield (offset, numVerts, numTris, offsetVertices) for each BOG v3 in a collection."""
    magic, version, total, count = struct.unpack_from('<4I', data)
    if magic != 0x43474f42 or version != 1 or total != len(data):
        raise ValueError('Unexpected BOG collection header')
    for o in struct.unpack_from(f'<{count}I', data, 16):
        if data[o:o + 4] != b'BOG ':
            raise ValueError('Missing BOG magic')
        h = struct.unpack_from('<10I', data, o + 4)
        ver, size, nv, nt, ov = h[0], h[1], h[5], h[6], h[9]
        if ver != 3 or 0x50 + 8 * nt > ov or ov + BOG_VERTEX * nv > size or o + size > len(data):
            raise ValueError('Unexpected BOG v3 layout')
        yield o, nv, nt, ov


def bake_bog(data, lightmaps, textures):
    """Return (new bog bytes, stats). lightmaps: material -> name; textures: name -> decoded."""
    out = bytearray(data)
    stats = dict(vertices=0, triangles=0, shared=0, missing_textures=set())
    for o, nv, nt, ov in bogs(data):
        samples = {}
        for i in range(nt):
            mat, *idx = struct.unpack_from('<4H', data, o + 0x50 + 8 * i)
            name = lightmaps.get(mat)
            if not name:
                continue
            if name not in textures:
                stats['missing_textures'].add(name)
                continue
            stats['triangles'] += 1
            for v in idx:
                if v >= nv:
                    raise ValueError('BOG index out of range')
                samples.setdefault(v, {})[name] = None
        for v, names in samples.items():
            base = o + ov + BOG_VERTEX * v
            u, t = (x * LIGHTMAP_UV_SCALE % 1.0 for x in struct.unpack_from('<2f', data, base + TEX2_OFFSET))
            lm = [0.0, 0.0, 0.0]
            for name in names:
                s = sample_bilinear(textures[name], u, t)
                lm = [a + b / len(names) for a, b in zip(lm, s)]
            old = struct.unpack_from('<3f', data, base + COLOR_OFFSET)
            struct.pack_into('<3f', out, base + COLOR_OFFSET, *(min(1.0, max(0.0, a + b)) for a, b in zip(old, lm)))
            stats['vertices'] += 1
            stats['shared'] += len(names) > 1
    stats['missing_textures'] = sorted(stats['missing_textures'])
    return bytes(out), stats


def bog_state(world, area):
    """'retail', 'baked', 'unknown', or None when the area is not covered."""
    if area not in BOG_HASHES:
        return None
    name, retail, baked = BOG_HASHES[area]
    verts = hot_entries(world).get('verts.hot')
    inner = hot_entries(read_entry(world, verts)) if verts else {}
    if name not in inner:
        return 'unknown'
    h = sha(read_entry(read_entry(world, verts), inner[name]))
    return {retail: 'retail', baked: 'baked'}.get(h, 'unknown')


def patch_world_hot(world, textures_hot, area):
    """Return (new world.hot bytes or None, report). Raises on unknown BOG data."""
    state = bog_state(world, area)
    if state is None:
        return None, {'state': 'not covered'}
    if state == 'baked':
        return None, {'state': 'already baked'}
    if state != 'retail':
        raise ValueError(f'{area}: BOG entry is neither retail nor baked; refusing to bake')
    outer = hot_entries(world)
    data = read_entry(world, outer['data.hot'])
    verts_raw = read_entry(world, outer['verts.hot'])
    tdf = next(read_entry(data, e) for n, e in hot_entries(data).items() if n.lower().endswith('index.tdf'))
    lightmaps = lightmapped_materials(tdf)
    tex_entries = {n.lower(): e for n, e in hot_entries(textures_hot).items()}
    textures = {n: decode_dds_rgb(read_entry(textures_hot, tex_entries[n])) for n in set(lightmaps.values()) if n in tex_entries}
    name = BOG_HASHES[area][0]
    inner = hot_entries(verts_raw)
    e = inner[name]
    if e['packed'] or e['hs']:
        raise ValueError('Expected an uncompressed BOG entry')
    old = read_entry(verts_raw, e)
    new, stats = bake_bog(old, lightmaps, textures)
    if stats['missing_textures']:
        raise ValueError(f'{area}: lightmaps missing from textures.hot: {stats["missing_textures"]}')
    verts_new = bytearray(verts_raw)
    verts_new[e['off']:e['off'] + len(new)] = new
    for n, entry in hot_entries(bytes(verts_new)).items():
        if read_entry(bytes(verts_new), entry) != (new if n == name else read_entry(verts_raw, inner[n])):
            raise ValueError(f'verts.hot round trip failed: {n}')
    stats['state'] = 'baked now'
    stats['bog_sha256'] = sha(new)
    stats['matches_reference'] = sha(new) == BOG_HASHES[area][2]
    return rebuild(world, {'verts.hot': bytes(verts_new)}), stats


# ---------------------------------------------------------------- commands

def levels(game):
    root = Path(game) / 'vincedata' / 'levels'
    if not root.is_dir():
        raise SystemExit(f'No vincedata/levels under {game}')
    return root


def status(game):
    root = levels(game)
    for area in sorted(BOG_HASHES):
        path = root / area / 'world.hot'
        print(f'{area}: {bog_state(path.read_bytes(), area) if path.exists() else "missing"}')


def stage(game, stage_dir, areas):
    root = levels(game)
    stage_dir = Path(stage_dir)
    if stage_dir.exists() and any(stage_dir.iterdir()):
        raise SystemExit('Choose an empty or new stage directory')
    manifest = {'files': {}}
    for area in sorted(areas or BOG_HASHES):
        if area not in BOG_HASHES:
            raise SystemExit(f'{area}: not an area this patch covers')
        path = root / area / 'world.hot'
        world = path.read_bytes()
        try:
            baked, report = patch_world_hot(world, (root / area / 'textures.hot').read_bytes(), area)
        except ValueError as e:
            raise SystemExit(str(e))
        if baked is None:
            print(f'{area}: {report["state"]}, skipped')
            continue
        rel = f'vincedata/levels/{area}/world.hot'
        out = stage_dir / 'files' / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(baked)
        manifest['files'][rel] = {'source_sha256': sha(world), 'baked_sha256': sha(baked), 'bogs': {BOG_HASHES[area][0]: report}}
        note = '' if report['matches_reference'] else ' (differs from reference bake: lightmaps were changed?)'
        print(f'{area}: {report["vertices"]} vertices baked{note}')
    if not manifest['files']:
        raise SystemExit('Nothing to bake')
    (stage_dir / 'manifest.json').write_text(json.dumps(manifest, indent=1))
    print(f'Staged {len(manifest["files"])} files in {stage_dir}')


def load_manifest(stage_dir):
    return json.loads((Path(stage_dir) / 'manifest.json').read_text())


def install(game, stage_dir):
    stage_dir = Path(stage_dir)
    files = load_manifest(stage_dir)['files']
    for rel, info in files.items():
        current = (Path(game) / rel).read_bytes()
        if sha(current) == info['baked_sha256']:
            continue
        if sha(current) != info['source_sha256']:
            raise SystemExit(f'{rel}: game file changed since staging; nothing installed')
        if sha((stage_dir / 'files' / rel).read_bytes()) != info['baked_sha256']:
            raise SystemExit(f'{rel}: staged file is damaged; nothing installed')
    for rel, info in files.items():
        target = Path(game) / rel
        if sha(target.read_bytes()) == info['baked_sha256']:
            continue
        backup = stage_dir / 'backup' / rel
        backup.parent.mkdir(parents=True, exist_ok=True)
        if not backup.exists():
            shutil.copyfile(target, backup)
        shutil.copyfile(stage_dir / 'files' / rel, target)
        print(f'Installed {rel}')


def restore(game, stage_dir):
    stage_dir = Path(stage_dir)
    files = load_manifest(stage_dir)['files']
    for rel, info in files.items():
        current = sha((Path(game) / rel).read_bytes())
        if current not in (info['baked_sha256'], info['source_sha256']):
            raise SystemExit(f'{rel}: game file is neither the baked nor the original version; nothing restored')
        backup = stage_dir / 'backup' / rel
        if current == info['baked_sha256'] and (not backup.exists() or sha(backup.read_bytes()) != info['source_sha256']):
            raise SystemExit(f'{rel}: no valid backup; nothing restored')
    for rel, info in files.items():
        target = Path(game) / rel
        if sha(target.read_bytes()) == info['baked_sha256']:
            shutil.copyfile(stage_dir / 'backup' / rel, target)
            print(f'Restored {rel}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=['status', 'stage', 'install', 'restore'])
    parser.add_argument('game', help='Voodoo Vince Remastered install folder')
    parser.add_argument('stage_dir', nargs='?')
    parser.add_argument('--area', action='append', help='Only this area, e.g. frenchquarter/area_courtyard (repeatable)')
    args = parser.parse_args()
    if args.command != 'status' and not args.stage_dir:
        parser.error(f'{args.command} needs a stage_dir')
    if args.command == 'status':
        status(args.game)
    elif args.command == 'stage':
        stage(args.game, args.stage_dir, set(args.area or ()))
    elif args.command == 'install':
        install(args.game, args.stage_dir)
    else:
        restore(args.game, args.stage_dir)


if __name__ == '__main__':
    main()
