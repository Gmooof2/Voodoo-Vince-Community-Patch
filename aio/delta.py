"""Binary delta: stock file -> built file. Stdlib only, deterministic.

Format (all integers little-endian):

  Header, 120 bytes:
    0   4  magic           b"VVD1"
    4   4  u32 version     1
    8   8  u64 source_size size of the stock file this delta applies to
    16  8  u64 target_size size of the file it produces
    24  32 sha256(source)  raw digest; apply refuses any other source
    56  32 sha256(target)  raw digest; apply verifies its result against it
    88  32 sha256(body)    raw digest of every byte after offset 120

  Body (offset 120 to end): a sequence of ops, nothing else.
    0x01 COPY : u8 0x01, u64 src_offset, u64 length   -> source[src_offset:src_offset+length]
    0x02 ADD  : u8 0x02, u64 length, then `length` literal bytes
  Ops are written to the output in order. Adjacent ADDs never occur (merged);
  COPYs shorter than MIN_COPY are folded into ADDs. A new file (no stock)
  uses source_size 0 and sha256(b"").

apply checks: magic, version, body sha, source size + sha, every op's bounds
(no read past the body or the source), result size + sha == header target.
Any failure raises DeltaError.

The ops are large contiguous reads, so a PowerShell 5.1 applier can work with
buffered stream reads/writes; no per-byte semantics are needed.
"""

import hashlib
import struct

MAGIC = b"VVD1"
VERSION = 1
HEADER = struct.Struct("<4sIQQ32s32s32s")  # 120 bytes
OP_COPY = 0x01
OP_ADD = 0x02
COPY = struct.Struct("<BQQ")
ADD = struct.Struct("<BQ")

BLOCK = 64          # stock index granularity (B)
MIN_COPY = 32       # shorter matches become ADD bytes
SHIFT_STRIDE = 16   # in unmatched regions, retry the previous copy's shift every N bytes
MAX_CHUNK = 1 << 20


class DeltaError(Exception):
    pass


def _fwd(s, t, sp, tp, limit_t):
    """Length of the common run s[sp:], t[tp:limit_t], by chunked slice compares."""
    n_max = min(len(s) - sp, limit_t - tp)
    length = 0
    c = 4096
    while length < n_max:
        n = min(c, n_max - length)
        if s[sp + length:sp + length + n] == t[tp + length:tp + length + n]:
            length += n
            if c < MAX_CHUNK:
                c *= 2
        elif n == 1:
            break
        else:
            c = n // 2
    return length


def _back(s, t, sp, tp, floor_t):
    """Length of the common run ending just before s[sp], t[tp], not below floor_t."""
    n_max = min(sp, tp - floor_t)
    length = 0
    c = 64
    while length < n_max:
        n = min(c, n_max - length)
        if s[sp - length - n:sp - length] == t[tp - length - n:tp - length]:
            length += n
            if c < MAX_CHUNK:
                c *= 2
        elif n == 1:
            break
        else:
            c = n // 2
    return length


def make(stock: bytes, target: bytes) -> bytes:
    s = bytes(stock)
    t = bytes(target)
    ls, lt = len(s), len(t)
    B = BLOCK

    index = {}
    for off in range(0, ls - B + 1, B):
        index.setdefault(s[off:off + B], off)  # first occurrence = lowest offset

    parts = []

    def emit_add(a, b):
        if b > a:
            parts.append(ADD.pack(OP_ADD, b - a))
            parts.append(t[a:b])

    add_start = 0
    pos = 0
    shift = None  # source offset - target offset of the previous copy
    while pos + B <= lt:
        cand = None
        if shift is not None and (pos - add_start) % SHIFT_STRIDE == 0:
            sp = pos + shift
            if 0 <= sp and sp + B <= ls and s[sp:sp + B] == t[pos:pos + B]:
                cand = sp
        if cand is None:
            cand = index.get(t[pos:pos + B])
        if cand is None:
            pos += 1
            continue
        f = _fwd(s, t, cand, pos, lt)
        b = _back(s, t, cand, pos, add_start)
        src, tgt, length = cand - b, pos - b, b + f
        if length < MIN_COPY:
            pos += 1
            continue
        emit_add(add_start, tgt)
        parts.append(COPY.pack(OP_COPY, src, length))
        pos = tgt + length
        add_start = pos
        shift = src - tgt
    emit_add(add_start, lt)

    body = b"".join(parts)
    head = HEADER.pack(MAGIC, VERSION, ls, lt,
                       hashlib.sha256(s).digest(),
                       hashlib.sha256(t).digest(),
                       hashlib.sha256(body).digest())
    return head + body


def make_from_runs(source: bytes, target: bytes, runs) -> bytes:
    """VVD1 delta whose COPYs are exactly the given shared runs [(target_offset, source_offset, length)] (aio/gate.py
    shared_runs, release.py public `derived`): a greedy cover by target offset (a run overlapping the bytes already
    covered keeps only its uncovered tail, at any length), every other byte ADD. Unlike make(), no stock run it is
    given is left inside an ADD."""
    s, t = bytes(source), bytes(target)
    parts = []
    pos = 0
    for to, so, n in sorted(runs):
        if to + n <= pos:
            continue
        if not (0 <= so and so + n <= len(s) and 0 <= to and to + n <= len(t) and s[so:so + n] == t[to:to + n]):
            raise DeltaError(f"run ({to}, {so}, {n}) is not shared by source and target")
        if to > pos:
            parts.append(ADD.pack(OP_ADD, to - pos) + t[pos:to])
        else:
            so, n, to = so + pos - to, n - (pos - to), pos
        parts.append(COPY.pack(OP_COPY, so, n))
        pos = to + n
    if pos < len(t):
        parts.append(ADD.pack(OP_ADD, len(t) - pos) + t[pos:])
    body = b"".join(parts)
    return HEADER.pack(MAGIC, VERSION, len(s), len(t), hashlib.sha256(s).digest(), hashlib.sha256(t).digest(),
                       hashlib.sha256(body).digest()) + body


def apply(stock: bytes, delta: bytes) -> bytes:
    if len(delta) < HEADER.size:
        raise DeltaError("delta shorter than header")
    magic, version, ssize, tsize, ssha, tsha, bsha = HEADER.unpack_from(delta, 0)
    if magic != MAGIC:
        raise DeltaError("bad magic")
    if version != VERSION:
        raise DeltaError(f"unsupported version {version}")
    body = memoryview(delta)[HEADER.size:]
    if hashlib.sha256(body).digest() != bsha:
        raise DeltaError("body sha256 mismatch")
    if len(stock) != ssize or hashlib.sha256(stock).digest() != ssha:
        raise DeltaError("source is not the stock file this delta was made from")
    out = []
    total = 0
    p, end = 0, len(body)
    while p < end:
        op = body[p]
        if op == OP_COPY:
            if p + COPY.size > end:
                raise DeltaError("truncated COPY")
            _, off, n = COPY.unpack_from(body, p)
            p += COPY.size
            if off + n > ssize:
                raise DeltaError("COPY out of source bounds")
            out.append(stock[off:off + n])
        elif op == OP_ADD:
            if p + ADD.size > end:
                raise DeltaError("truncated ADD")
            _, n = ADD.unpack_from(body, p)
            p += ADD.size
            if p + n > end:
                raise DeltaError("ADD out of body bounds")
            out.append(bytes(body[p:p + n]))
            p += n
        else:
            raise DeltaError(f"unknown op 0x{op:02x}")
        total += n
        if total > tsize:
            raise DeltaError("result exceeds target size")
    result = b"".join(out)
    if len(result) != tsize or hashlib.sha256(result).digest() != tsha:
        raise DeltaError("result does not match target sha256")
    return result
