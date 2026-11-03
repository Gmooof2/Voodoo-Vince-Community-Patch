"""Extract named DDS textures from the two HOT layouts and compare visually."""
from pathlib import Path
import io
import struct
import zlib


def entries(buf):
    if buf[:4] != b'HOT ':
        raise ValueError('Not HOT')
    names, count = struct.unpack_from('<II', buf, 20)
    stride = (names - 28) // count
    if stride not in (24, 32) or names != 28 + stride * count:
        raise ValueError('Unknown layout')
    result = {}
    for i in range(count):
        pos = 28 + stride * i
        vals = struct.unpack_from('<' + 'I' * (stride // 4), buf, pos)
        if stride == 24:
            name, hs, ho, raw, packed, off = vals
        else:
            name, _, hs, ho, raw, packed, off, _ = vals
        start = names + name
        name = buf[start:buf.index(0, start)].decode('latin1')
        result[name] = dict(pos=pos, hs=hs, ho=ho, raw=raw, packed=packed, off=off)
    return result


def read_entry(buf, e):
    header = buf[e['ho']:e['ho'] + e['hs']] if e['hs'] else b''
    size = e['raw'] - e['hs']
    payload = (zlib.decompress(buf[e['off']:e['off'] + e['packed']])
               if e['packed'] and e['packed'] != e['raw']
               else buf[e['off']:e['off'] + size])
    if len(payload) != size:
        raise ValueError('Unexpected payload size')
    return header, payload


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('game', type=Path, help='Game folder')
    game = parser.parse_args().game
    from PIL import Image, ImageDraw
    roots = [Path('Xbox - Voodoo Vince preview/levels/voodooshop/area_voodooshop'),
             game/'vincedata/levels/voodooshop/area_voodooshop']
    names = ['powericon.dds', 'envpowericon.dds', 'newpowericon_envmask.dds',
             'powerskull.dds', 'totemskull_base_envpowericon.dds']
    out = Path('research/texture_comparison')
    out.mkdir(parents=True, exist_ok=True)
    sheet = Image.new('RGB', (780, 310 * len(names)), '#282828')
    draw = ImageDraw.Draw(sheet)
    for col, root in enumerate(roots):
        buf = (root / 'textures.hot').read_bytes()
        es = entries(buf)
        for row, name in enumerate(names):
            header, data = read_entry(buf, es[name])
            dds = header + data
            label = ('Prototype' if col == 0 else 'Remastered')
            (out / (label + '_' + name)).write_bytes(dds)
            im = Image.open(io.BytesIO(dds)).convert('RGBA')
            im.save(out / (label + '_' + name + '.png'))
            print(label, name, im.size, header[84:88], len(data))
            draw.text((col * 390 + 10, row * 310 + 4), label + ': ' + name, fill='white')
            sheet.paste(im.resize((256, 256), Image.Resampling.NEAREST), (col * 390 + 10, row * 310 + 30), im.resize((256, 256)).getchannel('A'))
    sheet.save(out / 'comparison.png')
