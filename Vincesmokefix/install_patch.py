"""Install, check or remove Vincesmokefix (Kosmo's cabin smoke) in Vince.exe.

    python install_patch.py status --game "<game dir>"
    python install_patch.py apply  --game "<game dir>"
    python install_patch.py revert --game "<game dir>"

Standard library only. Close the game first. Byte replacements at three sites
listed in patchset.json, plus the PE checksum. `apply` installs only on the stock
Steam exe (whole-file sha256 STOCK_SHA256) and keeps a one-time backup
(Vince.exe.smokefix-v1-backup): written to a temp, flushed, re-hashed, then
renamed; an existing backup is reused only if it hashes to stock, else refused.
Vince.exe is swapped by temp + fsync + replace and re-read after the swap. A
site that is neither stock nor patched is refused. VinceFogSortFix hooks four
calls inside the frame-order site; with it installed, status reports that,
apply has nothing to do and revert refuses until VinceFogSortFix is reverted.

Exit codes: 0 ok, 1 refused, 2 bad input or the exe did not read back as written.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
STOCK_SHA256 = "f7e08e9bae9656de98b75fffc74bfc5960af8648f0a947ac65343934e5a35489"


class Refused(Exception):
    pass


def load_patchset():
    return json.loads((HERE / "patchset.json").read_text())


def sections(data):
    lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[lfanew:lfanew + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    count, optsize = struct.unpack_from("<H", data, lfanew + 6)[0], struct.unpack_from("<H", data, lfanew + 20)[0]
    timestamp = struct.unpack_from("<I", data, lfanew + 8)[0]
    image_base = struct.unpack_from("<I", data, lfanew + 24 + 28)[0]
    table = lfanew + 24 + optsize
    out = [struct.unpack_from("<IIII", data, table + 40 * i + 8) for i in range(count)]
    return lfanew, timestamp, image_base, out


def offset_of(data, va):
    _, _, image_base, secs = sections(data)
    rva = va - image_base
    for vsize, vaddr, rsize, raw in secs:
        if vaddr <= rva < vaddr + min(vsize, rsize):
            return raw + rva - vaddr
    raise ValueError(f"VA {va:#x} is not in the file")


def checksum(data):
    """The PE checksum (the same value as pefile's generate_checksum)."""
    lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    field = lfanew + 24 + 64
    total = 0
    padded = bytes(data) + b"\0" * (-len(data) % 4)
    for i in range(0, len(padded), 4):
        if i == field:
            continue
        total = (total & 0xFFFFFFFF) + struct.unpack_from("<I", padded, i)[0] + (total >> 32)
        if total > 2 ** 32:
            total = (total & 0xFFFFFFFF) + (total >> 32)
    total = (total & 0xFFFF) + (total >> 16)
    total = total + (total >> 16)
    return (total & 0xFFFF) + len(data)


def site_state(data, site, patchset):
    off = offset_of(data, site["va"])
    stock, patched = bytes.fromhex(site["stock_hex"]), bytes.fromhex(site["patched_hex"])
    current = data[off:off + len(stock)]
    if current == stock:
        return "stock"
    if current == patched:
        return "patched"
    for owner, overlay in patchset.get("overlays", {}).items():
        if overlay["site"] != site["name"]:
            continue
        # Same as patched except for the rel32 of the hooked calls.
        masked = bytearray(current)
        for va in overlay["call_vas"]:
            k = va - site["va"]
            if current[k] != 0xE8:
                break
            masked[k + 1:k + 5] = patched[k + 1:k + 5]
        else:
            if bytes(masked) == patched:
                return f"patched+{owner}"
    return "other"


def check_identity(data, patchset):
    _, timestamp, image_base, _ = sections(data)
    if timestamp != patchset["timestamp"] or image_base != patchset["image_base"]:
        raise ValueError("not the Steam Voodoo Vince Remastered executable this patch is for")


def status(data, patchset, identity=True):
    # identity=False (AIO build, phase 1.4): skip check_identity; sites are still compared
    if identity:
        check_identity(data, patchset)
    return {s["name"]: site_state(data, s, patchset) for s in patchset["sites"]}


def apply(data, patchset, identity=True):
    states = status(data, patchset, identity)
    bad = [n for n, st in states.items() if st == "other"]
    if bad:
        raise Refused(f"site(s) changed by something else: {', '.join(bad)}")
    todo = [s for s in patchset["sites"] if states[s["name"]] == "stock"]
    if not todo:
        return data
    result = bytearray(data)
    for s in todo:
        off = offset_of(data, s["va"])
        patched = bytes.fromhex(s["patched_hex"])
        result[off:off + len(patched)] = patched
    lfanew = struct.unpack_from("<I", result, 0x3C)[0]
    struct.pack_into("<I", result, lfanew + 24 + 64, checksum(result))
    return bytes(result)


def revert(data, patchset):
    states = status(data, patchset)
    layered = [f"{n} ({st.split('+', 1)[1]})" for n, st in states.items() if "+" in st]
    if layered:
        raise Refused("revert these first: " + ", ".join(layered))
    bad = [n for n, st in states.items() if st == "other"]
    if bad:
        raise Refused(f"site(s) changed by something else: {', '.join(bad)}")
    todo = [s for s in patchset["sites"] if states[s["name"]] == "patched"]
    if not todo:
        return data
    result = bytearray(data)
    for s in todo:
        off = offset_of(data, s["va"])
        stock = bytes.fromhex(s["stock_hex"])
        result[off:off + len(stock)] = stock
    lfanew = struct.unpack_from("<I", result, 0x3C)[0]
    struct.pack_into("<I", result, lfanew + 24 + 64, checksum(result))
    return bytes(result)


def write_synced(path, blob):
    with open(path, "wb") as f:
        f.write(blob)
        f.flush()
        os.fsync(f.fileno())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=["status", "apply", "revert"])
    parser.add_argument("--game", type=Path, required=True, help="game folder containing Vince.exe")
    args = parser.parse_args(argv)
    exe = args.game / "Vince.exe"
    try:
        patchset = load_patchset()
        data = exe.read_bytes()
        if args.action == "status":
            for name, st in status(data, patchset).items():
                print(f"{name:20} {st}")
            print("sha256", hashlib.sha256(data).hexdigest())
            return 0
        result = (apply if args.action == "apply" else revert)(data, patchset)
        if result == data:
            print("nothing to do")
            return 0
        backup = args.game / patchset["backup"]
        if args.action == "apply":
            if hashlib.sha256(data).hexdigest() != STOCK_SHA256:
                raise Refused("Vince.exe is not the stock Steam executable (sha256 "
                              f"{hashlib.sha256(data).hexdigest()}, expected {STOCK_SHA256})")
            if backup.exists():
                if hashlib.sha256(backup.read_bytes()).hexdigest() != STOCK_SHA256:
                    raise Refused(f"{backup.name} exists but is not the stock executable; move it away first")
            else:
                btmp = backup.with_name(backup.name + ".tmp")
                write_synced(btmp, data)
                if hashlib.sha256(btmp.read_bytes()).hexdigest() != STOCK_SHA256:
                    btmp.unlink()
                    raise OSError(f"{btmp.name} did not read back as written")
                os.replace(btmp, backup)
                print("backup", backup.name)
        tmp = exe.with_name(exe.name + ".smokefix-tmp")
        write_synced(tmp, result)
        os.replace(tmp, exe)
        if hashlib.sha256(exe.read_bytes()).hexdigest() != hashlib.sha256(result).hexdigest():
            print(f"error: Vince.exe did not read back as written; restore it from {backup.name}")
            return 2
        print(args.action, "done; sha256", hashlib.sha256(result).hexdigest())
        return 0
    except Refused as e:
        print("refused:", e)
        return 1
    except (OSError, ValueError) as e:
        print("error:", e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
