"""CatFix: on/off switch for quieter Land o' Rides cat ride sounds.

The five ride sounds (clr_ride01/02/03/05/07) live in
vincedata/levels/carnival/area_landorides/sounds.hot as 16-bit PCM. The game
plays them from code with no per-sound volume, so the switch scales their
samples in place. Only those five payloads change; every other byte of the
bank is kept, so this merges with other mods that touch the same archive.

  catfix.py status --game DIR
  catfix.py on     --game DIR [--gain-db -10]
  catfix.py off    --game DIR

Turning on saves the five original payloads under DIR/.CatFix/. Turning off
restores them and refuses if anything else has changed those payloads since.
Exit codes: 0 ok, 1 refused/conflict, 2 bad input.
"""
import argparse
import array
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

VERSION = 1
DEFAULT_GAIN_DB = -10.0
BANK = Path('vincedata/levels/carnival/area_landorides/sounds.hot')
LIST = BANK.with_name('sounds.lst')
STATE_DIR = '.CatFix'
SOUNDS = ('clr_ride01.wav', 'clr_ride02.wav', 'clr_ride03.wav',
          'clr_ride05.wav', 'clr_ride07.wav')

# Stock Steam 1.14.2.0 bank, before and after the approved -10 dB setting.
# Informational only: the switch works on any bank whose ride payloads parse.
STOCK_BANK_SHA256 = '9519add00e507d63302309cadd317c22563bcce532dc66eac4cb258b34cdb12b'
APPROVED_ON_SHA256 = '4551176fafcfcdb9879b81b2cf7daa831a16a03cc86152f10a04712dfa4a1acd'


class Refused(Exception):
    pass


def sha(blob):
    return hashlib.sha256(blob).hexdigest()


def parse_bank(data, names):
    """Return {name: (offset, size)} for every PCM payload in a HOT sound bank."""
    if data[:4] != b'HOT ':
        raise ValueError('not a HOT archive')
    count, = struct.unpack_from('<I', data, 0x18)
    if count != len(names):
        raise ValueError(f'bank has {count} sounds, list has {len(names)}')
    found = {}
    for i, name in enumerate(names):
        _, hdr, _, _, off, _, _, _ = struct.unpack_from('<8I', data, 0x24 + i * 32)
        if data[hdr:hdr + 4] != b'RIFF' or data[hdr + 8:hdr + 12] != b'WAVE':
            raise ValueError(f'{name}: no RIFF header')
        fmt, channels = struct.unpack_from('<HH', data, hdr + 20)
        bits, = struct.unpack_from('<H', data, hdr + 34)
        if data[hdr + 64:hdr + 68] != b'data' or fmt != 1 or bits != 16:
            raise ValueError(f'{name}: not 16-bit PCM')
        size, = struct.unpack_from('<I', data, hdr + 68)
        size -= size % 2
        if off + size > len(data):
            raise ValueError(f'{name}: payload past end of file')
        found[name] = (off, size)
    return found


def names_from_lst(text):
    return [line.strip().replace('\\', '/').split('/')[-1]
            for line in text.splitlines() if line.strip()]


def read_names(path):
    return names_from_lst(path.read_text())


def scale(payload, gain_db):
    gain = 10 ** (gain_db / 20)
    samples = array.array('h')
    samples.frombytes(payload)
    if sys.byteorder != 'little':
        samples.byteswap()
    for i, x in enumerate(samples):
        samples[i] = max(-32768, min(32767, round(x * gain)))
    if sys.byteorder != 'little':
        samples.byteswap()
    return samples.tobytes()


def apply(bank, names, gain_db=DEFAULT_GAIN_DB):
    """Pure: the bank with the five ride payloads scaled by gain_db; every other byte kept."""
    table = parse_bank(bank, names)
    missing = [s for s in SOUNDS if s not in table]
    if missing:
        raise ValueError(f'ride sounds missing from bank: {missing}')
    data = bytearray(bank)
    for name in SOUNDS:
        off, size = table[name]
        data[off:off + size] = scale(bytes(data[off:off + size]), gain_db)
    return bytes(data)


def patch_sounds_hot(bank, lst_bytes):
    """aio/build.py data-chain adapter: (sounds.hot, sounds.lst) -> (patched bank, {})."""
    return apply(bank, names_from_lst(lst_bytes.decode('latin-1')), DEFAULT_GAIN_DB), {}


def write_atomic(path, blob):
    tmp = path.with_name(path.name + '.catfix-tmp')
    tmp.write_bytes(blob)
    os.replace(tmp, path)


class Game:
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.bank = self.root / BANK
        self.state_dir = self.root / STATE_DIR
        self.state_file = self.state_dir / 'state.json'
        if not self.bank.is_file() or not (self.root / LIST).is_file():
            raise ValueError(f'Land o\' Rides sound bank not found under {self.root}')
        self.data = bytearray(self.bank.read_bytes())
        table = parse_bank(self.data, read_names(self.root / LIST))
        missing = [s for s in SOUNDS if s not in table]
        if missing:
            raise ValueError(f'ride sounds missing from bank: {missing}')
        self.slots = {s: table[s] for s in SOUNDS}

    def payload(self, name):
        off, size = self.slots[name]
        return bytes(self.data[off:off + size])

    def put(self, name, blob):
        off, size = self.slots[name]
        assert len(blob) == size
        self.data[off:off + size] = blob

    def state(self):
        if not self.state_file.is_file():
            return None
        return json.loads(self.state_file.read_text())

    def status(self):
        """'off', 'on', or 'conflict' plus a human-readable detail."""
        st = self.state()
        if st is None:
            return 'off', 'stock ride sounds (no CatFix state)'
        current = {s: sha(self.payload(s)) for s in SOUNDS}
        if all(current[s] == st['sounds'][s]['patched_sha256'] for s in SOUNDS):
            return 'on', f'ride sounds at {st["gain_db"]:+g} dB'
        if all(current[s] == st['sounds'][s]['original_sha256'] for s in SOUNDS):
            return 'off', 'stock ride sounds (stale CatFix state, e.g. after Steam verify)'
        changed = [s for s in SOUNDS if current[s] != st['sounds'][s]['patched_sha256']]
        return 'conflict', f'ride sounds changed outside CatFix: {", ".join(changed)}'


def turn_on(game, gain_db):
    status, detail = game.status()
    st = game.state()
    if status == 'conflict':
        raise Refused(detail)
    if status == 'on':
        if st['gain_db'] == gain_db:
            return f'already on ({detail})'
        turn_off(game)
        game = Game(game.root)
    elif st is not None:
        # Stale state: payloads are stock again, so start over cleanly.
        clear_state(game)
    game.state_dir.mkdir(exist_ok=True)
    backup = game.state_dir / 'backup'
    backup.mkdir(exist_ok=True)
    before = sha(game.data)
    originals = {name: game.payload(name) for name in SOUNDS}
    game.data = bytearray(apply(bytes(game.data), read_names(game.root / LIST), gain_db))
    record = {}
    for name in SOUNDS:
        original, patched = originals[name], game.payload(name)
        write_atomic(backup / (name + '.pcm'), original)
        off, size = game.slots[name]
        record[name] = {'offset': off, 'size': size,
                        'original_sha256': sha(original),
                        'patched_sha256': sha(patched)}
    state = {'tool': 'CatFix', 'version': VERSION, 'gain_db': gain_db,
             'bank': BANK.as_posix(), 'bank_sha256_before': before,
             'bank_sha256_after': sha(game.data), 'sounds': record}
    # State first: if the bank write fails, status still sees stock payloads.
    write_atomic(game.state_file, json.dumps(state, indent=2).encode())
    write_atomic(game.bank, bytes(game.data))
    return f'on: ride sounds at {gain_db:+g} dB'


def turn_off(game):
    status, detail = game.status()
    st = game.state()
    if status == 'conflict':
        raise Refused(detail)
    if status == 'off':
        if st is not None:
            clear_state(game)
        return f'already off ({detail})'
    backup = game.state_dir / 'backup'
    for name in SOUNDS:
        original = (backup / (name + '.pcm')).read_bytes()
        if sha(original) != st['sounds'][name]['original_sha256']:
            raise Refused(f'backup for {name} is damaged')
        game.put(name, original)
    write_atomic(game.bank, bytes(game.data))
    clear_state(game)
    return 'off: stock ride sounds restored'


def clear_state(game):
    backup = game.state_dir / 'backup'
    for name in SOUNDS:
        (backup / (name + '.pcm')).unlink(missing_ok=True)
    game.state_file.unlink(missing_ok=True)
    for d in (backup, game.state_dir):
        try:
            d.rmdir()
        except OSError:
            pass


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('mode', choices=('status', 'on', 'off'))
    ap.add_argument('--game', required=True, help='Voodoo Vince Remastered folder')
    ap.add_argument('--gain-db', type=float, default=DEFAULT_GAIN_DB,
                    help='ride sound change in dB when turning on (default -10)')
    args = ap.parse_args(argv)
    if not -60 <= args.gain_db < 0:
        ap.error('--gain-db must be between -60 and 0')
    try:
        game = Game(args.game)
        if args.mode == 'status':
            status, detail = game.status()
            print(f'{status}: {detail}')
            return 1 if status == 'conflict' else 0
        print(turn_on(game, args.gain_db) if args.mode == 'on' else turn_off(game))
        return 0
    except Refused as e:
        print(f'refused: {e}', file=sys.stderr)
        return 1
    except (ValueError, OSError, struct.error) as e:
        print(f'error: {e}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
