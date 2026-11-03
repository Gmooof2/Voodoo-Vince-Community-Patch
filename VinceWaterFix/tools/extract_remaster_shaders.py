"""Inspect locally owned Remastered binaries; extracted game code stays private.

Usage: python tools/extract_remaster_shaders.py PATH_TO_VINCE_EXE
No executable modifications are performed. Requires Windows for disassembly.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import struct


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def containers(data):
    offset = 0
    while (offset := data.find(b'DXBC', offset)) >= 0:
        if offset + 32 <= len(data):
            size, count = struct.unpack_from('<II', data, offset + 24)
            if 0 < count < 32 and 32 + count * 4 <= size <= len(data) - offset:
                blob = data[offset:offset + size]
                chunks = {}
                for index in range(count):
                    start = u32(blob, 32 + index * 4)
                    if start > size - 8:
                        break
                    length = u32(blob, start + 4)
                    if start + 8 + length > size:
                        break
                    chunks[blob[start:start + 4].decode('ascii')] = blob[start + 8:start + 8 + length]
                else:
                    yield offset, blob, chunks
        offset += 4


def pdb_streams(data):
    """Reconstruct MSF 7 streams, so sources crossing blocks are intact."""
    if not data.startswith(b'Microsoft C/C++ MSF 7.00'):
        return []
    block_size, _, _, directory_size, _, block_map = struct.unpack_from('<6I', data, 32)
    count = (directory_size + block_size - 1) // block_size
    directory_blocks = struct.unpack_from(f'<{count}I', data, block_map * block_size)
    directory = b''.join(data[b * block_size:(b + 1) * block_size] for b in directory_blocks)[:directory_size]
    stream_count = u32(directory, 0)
    sizes = struct.unpack_from(f'<{stream_count}I', directory, 4)
    position = 4 + 4 * stream_count
    streams = []
    for size in sizes:
        if size == 0xFFFFFFFF:
            streams.append(b'')
            continue
        count = (size + block_size - 1) // block_size
        blocks = struct.unpack_from(f'<{count}I', directory, position)
        position += count * 4
        streams.append(b''.join(data[b * block_size:(b + 1) * block_size] for b in blocks)[:size])
    return streams


def disassemble(blob):
    compiler = ctypes.WinDLL('d3dcompiler_47.dll')
    function = compiler.D3DDisassemble
    function.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    function.restype = ctypes.c_long
    source = ctypes.create_string_buffer(blob)
    result = ctypes.c_void_p()
    status = function(source, len(blob), 0, None, ctypes.byref(result))
    if status < 0:
        raise OSError(f'D3DDisassemble failed: {status:#x}')
    table = ctypes.cast(result, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    pointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(table[3])(result)
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(table[4])(result)
    text = ctypes.string_at(pointer, size).rstrip(b'\0').decode('utf-8', errors='replace')
    ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(table[2])(result)
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--output', type=Path, default=Path('research/private/remaster-shaders'))
    args = parser.parse_args()
    data = args.executable.read_bytes()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = {'executable_sha256': hashlib.sha256(data).hexdigest(), 'shaders': []}
    for index, (offset, blob, chunks) in enumerate(containers(data)):
        prefix = args.output / f'{index:02d}'
        prefix.with_suffix('.dxbc').write_bytes(blob)
        assembly = disassemble(blob)
        prefix.with_suffix('.asm').write_text(assembly, encoding='utf-8', newline='\n')
        sources = []
        for stream_index, stream in enumerate(pdb_streams(chunks.get('SPDB', b''))):
            # A source stream can have a small binary header before UTF-8 text.
            for match in re.finditer(rb'[\x09\x0a\x0d\x20-\x7e]{160,}', stream):
                text = match.group().decode('ascii').replace('\r\n', '\n')
                if any(token in text for token in ('float4', 'Texture2D', 'cbuffer')):
                    name = f'{index:02d}-stream{stream_index}-{match.start()}.hlsl'
                    (args.output / name).write_text(text, encoding='utf-8', newline='\n')
                    sources.append(name)
        code = chunks.get('SHDR', chunks.get('SHEX', b''))
        profile = re.search(r'^([pvghdc]s_\d_\d)\s*$', assembly, re.MULTILINE)
        source_files = sorted(set(re.findall(r'#line \d+ "([^"]+)"', assembly)))
        manifest['shaders'].append({'index': index, 'file_offset': hex(offset), 'size': len(blob),
                                    'sha256': hashlib.sha256(blob).hexdigest(),
                                    'profile': profile.group(1) if profile else None,
                                    'source_files': source_files,
                                    'code_sha256': hashlib.sha256(code).hexdigest(),
                                    'chunks': {k: len(v) for k, v in chunks.items()}, 'sources': sources})
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(f"Extracted {len(manifest['shaders'])} shaders to {args.output}")


if __name__ == '__main__':
    main()
