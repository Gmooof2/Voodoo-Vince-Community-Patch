"""Read-only LSBL inspection and pure byte-preserving replacement helper.
No game writes. replace_text returns bytes for a separate installer to validate/save.
"""
from pathlib import Path
import struct

class LSBL:
    def __init__(self, raw):
        self.raw = bytes(raw)
        if len(raw) < 42 or raw[:4] != b'LSBL':
            raise ValueError('Not an LSBL string blob')
        if struct.unpack_from('<6I', raw, 4) != (28, 4, 28, 0, 0, 0):
            raise ValueError('Unsupported LSBL header')
        self.block_size, self.count = struct.unpack_from('<II', raw, 28)
        if self.block_size != len(raw) - 28:
            raise ValueError('Inconsistent LSBL block length')
        self.text_start = 36 + 6 * (self.count + 1)
        if self.text_start > len(raw):
            raise ValueError('Truncated LSBL index')
        self.rows = [struct.unpack_from('<HI', raw, 36 + i * 6)
                     for i in range(self.count + 1)]
        if self.rows[-1][0] != 65535 or self.rows[0][1] != 0:
            raise ValueError('Missing sentinel or nonzero first text offset')
        if any(a[0] >= b[0] or a[1] >= b[1]
               for a,b in zip(self.rows, self.rows[1:])):
            raise ValueError('Unordered or duplicate LSBL records')
        self.text_end = self.text_start + self.rows[-1][1] * 2
        if self.text_end > len(raw):
            raise ValueError('LSBL text exceeds file')
        for i in range(self.count):
            data = self.entry_bytes(i)
            if not data.endswith(b'\0\0'):
                raise ValueError('Nonterminated LSBL string')
            data.decode('utf-16le')

    def entry_bytes(self, index):
        start = self.text_start + self.rows[index][1] * 2
        end = self.text_start + self.rows[index + 1][1] * 2
        return self.raw[start:end]

    def text(self, identifier):
        i = next(i for i,r in enumerate(self.rows[:-1]) if r[0] == identifier)
        return self.entry_bytes(i).decode('utf-16le')[:-1]

    def replace_text(self, identifier, text, expected=None):
        if '\0' in text:
            raise ValueError('Embedded NUL in replacement')
        i = next(i for i,r in enumerate(self.rows[:-1]) if r[0] == identifier)
        if expected is not None and self.text(identifier) != expected:
            raise ValueError('Unexpected original subtitle')
        replacement = text.encode('utf-16le') + b'\0\0'
        old = self.entry_bytes(i)
        delta = len(replacement) - len(old)
        start = self.text_start + self.rows[i][1] * 2
        end = start + len(old)
        out = bytearray(self.raw[:start] + replacement + self.raw[end:])
        struct.pack_into('<I', out, 28, self.block_size + delta)
        for j in range(i + 1, self.count + 1):
            struct.pack_into('<I', out, 36 + j * 6 + 2,
                             self.rows[j][1] + delta // 2)
        result = LSBL(out)
        assert result.text(identifier) == text
        assert result.rows[i][1] == self.rows[i][1]
        assert result.raw[result.text_end:] == self.raw[self.text_end:]
        assert all(result.entry_bytes(j) == self.entry_bytes(j)
                   for j in range(self.count) if j != i)
        return bytes(out)

if __name__ == '__main__':
    import sys
    source = Path(sys.argv[1])
    blob = LSBL(source.read_bytes())
    identifier = int(sys.argv[2]) if len(sys.argv) > 2 else 1008
    print('Entries:', blob.count, 'text start:', hex(blob.text_start))
    print(repr(blob.text(identifier)))
    # Pure in-memory non-BMP replacement exercises code-unit shifting.
    old = blob.text(identifier)
    assert blob.replace_text(identifier, old, expected=old) == blob.raw
    changed = LSBL(blob.replace_text(identifier, old + ' [probe \U0001f600]', expected=old))
    assert changed.replace_text(identifier, old) == blob.raw
    print('Byte-identical no-op and reversible Unicode replacement checks passed.')
