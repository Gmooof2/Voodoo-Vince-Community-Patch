"""Make a separate, experimental no-output crash fix; never overwrites its input.

Usage: python prepare_patch.py --input Vince.exe [--output build/Vince.audio-guard.exe]   (--input required, 1.3 T5)
Adds a position-independent readiness guard to the streaming initializer.
This is a narrowly scoped binary fix, not an audio-device recovery system.
"""
import argparse
import hashlib
import struct
from pathlib import Path

STREAM_RVA = 0x5CA90
GLOBAL_GAME_RVA = 0x7653F4
ORIGINAL = bytes.fromhex('55 8b ec 81 ec c4 05 00 00')
WAIT_CALL_RVA = 0xD1336
WAIT_RESUME_RVA = 0xD133B
WAIT_EXIT_RVA = 0xD13CB
THREAD_SETUP_RVA = 0x5D0AA
THREAD_SETUP = bytes.fromhex('8b f0 6a 01 56')
LATE_COUNT_RVA = 0x5D100
LATE_COUNT = bytes.fromhex('b8 01 00 00 00 87 07')

def publish_code(rva):
    """Publish the worker count while the new thread is still suspended.

    EAX initially holds the CreateThread handle; consume it into ESI before
    using EAX as the xchg scratch register. EDI points at stream+0xC. Then
    replay the SetThreadPriority arguments and resume the original launcher.
    The old publication AFTER ResumeThread must also be removed.
    """
    code = bytearray(THREAD_SETUP[:2])
    code += LATE_COUNT
    code += THREAD_SETUP[2:]
    code += b'\xe9' + struct.pack('<i', THREAD_SETUP_RVA+len(THREAD_SETUP)-(rva+len(code)+5))
    return bytes(code)

def wait_code(rva):
    """Replacement for the caller's call site (entered with JMP).

    Call the guarded initializer, then honor its Boolean result. A failed
    stream can never submit its first buffer, so waiting would hang forever.
    Relative branches preserve ASLR and do not rewrite return addresses.
    """
    code = bytearray(b'\xe8' + struct.pack('<i', STREAM_RVA - (rva + 5)))
    code += b'\x84\xc0'  # test al,al
    code += b'\x0f\x84' + struct.pack('<i', WAIT_EXIT_RVA - (rva + len(code) + 6))
    code += b'\xe9' + struct.pack('<i', WAIT_RESUME_RVA - (rva + len(code) + 5))
    return bytes(code)

def guard_code(rva):
    """ECX (stream this) is untouched. EAX/flags are caller-saved on x86.

    call/pop obtains the relocated image address without adding PE relocations.
    A missing game, audio manager, engine, or master returns false before the
    stream's initialized flag or any stream resources have changed.
    """
    code = bytearray(b'\xe8\0\0\0\0\x58')   # call $+5; pop eax
    code += b'\x8b\x80' + struct.pack('<i', GLOBAL_GAME_RVA - (rva + 5))
    jumps = []
    def require_nonzero(opcode):
        code.extend(opcode)
        jumps.append(len(code))
        code.extend(b'\x74\x00')
    require_nonzero(b'\x85\xc0')               # test eax,eax (game)
    code += b'\x8b\x40\x38'                  # game->audio
    require_nonzero(b'\x85\xc0')
    require_nonzero(b'\x83\x78\x48\x00')     # audio->engine
    require_nonzero(b'\x83\x78\x4c\x00')     # audio->master
    code += ORIGINAL
    end_jump = rva + len(code)
    code += b'\xe9' + struct.pack('<i', STREAM_RVA + len(ORIGINAL) - (end_jump + 5))
    fail = len(code)
    code += b'\x31\xc0\xc3'                  # xor eax,eax; ret (no arguments)
    for jump in jumps:
        delta = fail - (jump + 2)
        assert 0 <= delta < 128
        code[jump + 1] = delta
    return bytes(code)

def align(value, alignment):
    if not alignment or alignment & (alignment-1):
        raise ValueError('Invalid PE alignment')
    return (value + alignment-1) & -alignment

# Also verify the layout the guard follows, and the source-voice call implicated by the dump.
SIGNATURES = (
    (0x5CD9A, bytes.fromhex('a1 f4 53 b6 00')),
    (0x5CDA3, bytes.fromhex('8b 40 38 8b 48 48')),
    (0x5CDBF, bytes.fromhex('ff 52 20 8b 4b 10')),
    (0x42F38, bytes.fromhex('ff 50 28 85 c0 79 18')),
    (WAIT_CALL_RVA, b'\xe8' + struct.pack('<i', STREAM_RVA - (WAIT_CALL_RVA + 5))),
    (WAIT_RESUME_RVA, bytes.fromhex('8b 0d 20 32 b6 00')),
    (WAIT_EXIT_RVA, bytes.fromhex('8b e5 5d c3')),
    (THREAD_SETUP_RVA, THREAD_SETUP),
    (LATE_COUNT_RVA, LATE_COUNT),
    (0x5D0B5, bytes.fromhex('56 ff 15 dc 60 6d 00')),
)
VACFIX_NAME = b'.vacfix'
VACFIX_CHARACTERISTICS = 0x60000020
# (field, op, value); a str value names a property of the new section (header_apply values).
HEADER_EDITS = (('NumberOfSections','add',1),('SizeOfCode','add','SizeOfRawData'),
                ('SizeOfImage','align','end'),('CheckSum','set',0)) # CheckSum: optional image checksum
HEADER_FIELDS = {'NumberOfSections':(6,'<H'),'SizeOfCode':(24+4,'<I'),'SizeOfInitializedData':(24+8,'<I'),
                 'SizeOfImage':(24+56,'<I'),'CheckSum':(24+64,'<I'),
                 'DataDirectory[1].VirtualAddress':(24+104,'<I'),'DataDirectory[1].Size':(24+108,'<I')}

def header_apply(data, pe, edits, values, section_align):
    """Apply (field, op, value) header edits; offsets from the PE signature, str values looked up in values."""
    for field, op, value in edits:
        at, fmt = HEADER_FIELDS[field]
        v = values[value] if isinstance(value, str) else value
        if op == 'add':
            v += struct.unpack_from(fmt, data, pe+at)[0]
        elif op == 'align':
            v = align(v, section_align)
        elif op != 'set':
            raise ValueError(f'Unknown header op {op}')
        struct.pack_into(fmt, data, pe+at, v)

def vacfix_code(new_rva):
    """The .vacfix section bytes at new_rva and the RVA of each piece."""
    code = guard_code(new_rva)
    wait_rva = new_rva + align(len(code),16)
    code += bytes(wait_rva - new_rva - len(code)) + wait_code(wait_rva)
    publish_rva = new_rva + align(len(code),16)
    code += bytes(publish_rva - new_rva - len(code)) + publish_code(publish_rva)
    return code, {'guard':new_rva,'wait':wait_rva,'publish':publish_rva}

def site_edits(entries):
    """(rva, before, after) of each game site, jumping to the vacfix_code entries."""
    return [(STREAM_RVA, ORIGINAL, b'\xe9'+struct.pack('<i',entries['guard']-STREAM_RVA-5)+b'\x90'*4),
            (WAIT_CALL_RVA, b'\xe8' + struct.pack('<i', STREAM_RVA - (WAIT_CALL_RVA + 5)), b'\xe9' + struct.pack('<i', entries['wait']-WAIT_CALL_RVA-5)),
            (THREAD_SETUP_RVA, THREAD_SETUP, b'\xe9' + struct.pack('<i', entries['publish']-THREAD_SETUP_RVA-5)),
            (LATE_COUNT_RVA, LATE_COUNT, b'\x90'*len(LATE_COUNT))]

def patch(data):
    data = bytearray(data)
    if data[:2] != b'MZ':
        raise ValueError('Not a PE file')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe+4] != b'PE\0\0':
        raise ValueError('Invalid PE signature')
    machine, count = struct.unpack_from('<HH', data, pe+4)
    opt_size = struct.unpack_from('<H', data, pe+20)[0]
    opt = pe + 24
    if machine != 0x14c or struct.unpack_from('<H',data,opt)[0] != 0x10b:
        raise ValueError('Expected a 32-bit x86 executable')
    section_align, file_align = struct.unpack_from('<II',data,opt+32)
    table = opt + opt_size
    sections = []
    for i in range(count):
        at = table + i*40
        size, rva, raw_size, raw = struct.unpack_from('<IIII', data, at+8)
        sections.append((rva,size,raw,raw_size))
        if data[at:at+8].rstrip(b'\0') == VACFIX_NAME:
            raise ValueError('Already patched')
    def offset(rva):
        for start,size,raw,raw_size in sections:
            if start <= rva < start+raw_size:
                return raw+rva-start
        raise ValueError(f'RVA 0x{rva:x} is not backed by file data')
    site = offset(STREAM_RVA)
    if data[site:site+len(ORIGINAL)] != ORIGINAL:
        raise ValueError('Unsupported stream initializer bytes; no patch written')
    # Also verify the layout the guard follows, and the source-voice call implicated by the dump (SIGNATURES).
    for rva, signature in SIGNATURES:
        at = offset(rva)
        if data[at:at+len(signature)] != signature:
            raise ValueError(f'Unsupported audio code at RVA 0x{rva:x}')
    header = table + count*40
    if header+40 > min(raw for _,_,raw,size in sections if size):
        raise ValueError('No space for another PE section header')
    if any(data[header:header+40]):
        raise ValueError('New section header would overwrite existing data')
    if any(struct.unpack_from('<II',data,opt+96+4*8)):
        raise ValueError('Input has a certificate table; refusing to invalidate a signature')
    new_rva = align(max(rva+max(size,raw_size) for rva,size,raw,raw_size in sections),section_align)
    code, entries = vacfix_code(new_rva)
    new_raw = align(len(data),file_align)
    new_size = align(len(code),file_align)
    data.extend(bytes(new_raw+new_size-len(data)))
    data[new_raw:new_raw+len(code)] = code
    struct.pack_into('<8sIIIIIIHHI',data,header,VACFIX_NAME,len(code),new_rva,new_size,new_raw,0,0,0,0,VACFIX_CHARACTERISTICS)
    header_apply(data, pe, HEADER_EDITS, {'rva':new_rva,'end':new_rva+len(code),'SizeOfRawData':new_size}, section_align)
    for rva, before, after in site_edits(entries):
        at = offset(rva)
        data[at:at+len(after)] = after
    return bytes(data), new_rva, code

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',type=Path,required=True)  # 1.3 T5: was a default to the developer's Steam game folder Vince.exe (S-vinceaudio-05)
    parser.add_argument('--output',type=Path,default=Path(__file__).parent/'build'/'Vince.audio-guard.exe')
    args=parser.parse_args()
    if args.input.resolve()==args.output.resolve() or args.output.exists():
        parser.error('Output must be a new, separate file')
    before=args.input.read_bytes()
    after,rva,code=patch(before)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_bytes(after)
    print(f'Input SHA256:  {hashlib.sha256(before).hexdigest()}')
    print(f'Output SHA256: {hashlib.sha256(after).hexdigest()}')
    print(f'Guard: {len(code)} bytes at RVA 0x{rva:X}')
    print(f'Experimental test copy: {args.output}')
    print('Installed game has not been modified. Gameplay with no output device is not yet tested.')

if __name__=='__main__':
    main()
