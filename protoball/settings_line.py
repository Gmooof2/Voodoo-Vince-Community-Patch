"""protoball: vincedata/settings.tdf as a one-line edit (S-protoball-01, 1.3 T16).

Pure functions on bytes; no file I/O. The 1.5 data build calls apply() at install and
revert() at uninstall; 1.7's installer is the only writer of game files.

apply(data)            -> (new_data, old_line): requires the line ZGC_GBCML_LIGHT_COLOR to read
                          the stock value 200,200,500 (stock file f17521ec has 200.000000,...);
                          replaces only that value with the --light 0.3 result 60,60,150, written
                          in protoball.py's '%f' format (installed file = protoball.py's db9c76af).
                          old_line is the full line (no newline) for the install record.
revert(data, old_line) -> data with the line restored, only while it still reads 60,60,150;
                          refuses otherwise (a player or another tool changed it).
state(data)            -> 'stock' | 'aio' | 'other'.
Every other byte, LF endings included, is kept. Refusals raise SettingsLineError.
"""
import hashlib
import re

KEY = b'ZGC_GBCML_LIGHT_COLOR'
STOCK_SHA256 = 'f17521ec2cbabe6931a0bb8bae76aa00d31fe53a167e1b513986f83ea5db6595'
STOCK_VALUE = (200.0, 200.0, 500.0)
LIGHT = 0.3  # protoball.py --light 0.3 (protoball/README.md current setting)
AIO_VALUE = (60.0, 60.0, 150.0)
LINE_RE = re.compile(rb'^(ZGC_GBCML_LIGHT_COLOR[ \t]*=[ \t]*)([^;\r\n]+)(;[^\r\n]*)(?=\r?$)', re.M)


class SettingsLineError(ValueError):
    pass


def _find(data):
    ms = list(LINE_RE.finditer(data))
    if len(ms) != 1:
        raise SettingsLineError('settings.tdf: %d %s lines, need exactly 1' % (len(ms), KEY.decode()))
    return ms[0]


def _values(m):
    try:
        return tuple(float(v) for v in m.group(2).split(b','))
    except ValueError:
        raise SettingsLineError('settings.tdf: unreadable value %r' % m.group(2))


def state(data):
    v = _values(_find(data))
    return 'stock' if v == STOCK_VALUE else 'aio' if v == AIO_VALUE else 'other'


def apply(data):
    m = _find(data)
    stock_file = hashlib.sha256(data).hexdigest() == STOCK_SHA256
    if not stock_file and _values(m) != STOCK_VALUE:
        raise SettingsLineError('settings.tdf: %s reads %r, not the stock 200,200,500; refusing'
                                % (KEY.decode(), m.group(2)))
    value = ','.join('%f' % (v * LIGHT) for v in STOCK_VALUE).encode()
    assert tuple(float(v) for v in value.split(b',')) == AIO_VALUE
    new = data[:m.start(2)] + value + data[m.end(2):]
    return new, m.group(0)


def revert(data, old_line):
    m = _find(data)
    if _values(m) != AIO_VALUE:
        raise SettingsLineError('settings.tdf: %s reads %r, not the AIO 60,60,150; refusing to restore'
                                % (KEY.decode(), m.group(2)))
    old = LINE_RE.fullmatch(old_line)
    if not old or tuple(float(v) for v in old.group(2).split(b',')) != STOCK_VALUE:
        raise SettingsLineError('recorded line %r is not a stock %s line; refusing' % (old_line, KEY.decode()))
    return data[:m.start()] + old_line + data[m.end():]
