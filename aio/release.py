"""Release packager: build every AIO output from the oracle and pack build/release/<ZIP_NAME>
(VoodooVinceCommunityPatch-<VERSION>.zip).

zip --stock S [--components P]: refuse (exit 1, nothing built) when any aio/components.json record (P overrides the
path, for tests) is release_held; empty build/release/; run build.py exe, dll, data, options with --out
build/release/work one after another (C0050); refuse on a non-zero exit, a held/ dir or a held:true ownership record.
Package: X3DAudio1_7.dll and d3d11.dll whole, every other changed game file (always-on exe + data, options/<set>/
files) as a VVD1 delta (aio/delta.py) from its stock bytes (b"" when absent from stock) at deltas/<result sha>.vvd,
VinceAIO.ini, known_releases.json (aio/, earlier releases the installer upgrades), licences/, aio/installer/ (under
installer/) and aio/dist/ (at the root) when they exist, and
manifest.json: version, target exe sha, stock sha per touched game path, the 8 option sets in OPTIONS mask order
with their resolved files, the AIO's own files with shas. Every stock sha is checked against aio/stock_manifest.json.
The zip is deterministic (sorted members, fixed date_time and attributes, deflate level 9); build/release/work is
removed after zipping unless --keep-work. Exit 0 ok, 1 refused, 2 bad input (as build.py).

public: write build/public/, the clean public source tree, from the allowlist aio/public_tree.json (entries at their
repo path, dir entries recursive minus __pycache__ and the exclude list, map entries from -> to) plus
aio/stock_manifest.json without its "source" key (the developer's oracle path). Refuse (exit 1) when a
release_held component's folder or a dev_only path is covered, or a shipped file is empty. Each public_tree.json
`derived` entry {path, stock} (a file sharing byte runs with a stock file) ships as <path>.vvd instead: a VVD1 delta from
<stock> (--stock, default env VV_ORACLE else the manifest "source") whose COPYs are exactly the runs aio/gate.py
shared_runs finds, everything else ADD; the list goes to aio/derived.json.

restore --stock S: write each derived <path> from <path>.vvd and S/<stock> (delta.apply checks both shas; exit 1 on a
wrong stock), in the tree this release.py lives in. zip runs it first when a derived <path> is missing.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import delta  # noqa: E402
from build import OPTIONS, option_set  # noqa: E402

VERSION = "2.0.0"  # the one version source
PRODUCT = "Voodoo Vince Community Patch"  # the one player-facing name (manifest name, installer text)
COMPONENTS = HERE / "components.json"
STOCK_MANIFEST = HERE / "stock_manifest.json"
OUT = ROOT / "build" / "release"
WORK = OUT / "work"
ZIP_NAME = f"{PRODUCT.replace(' ', '')}-{VERSION}.zip"
PUBLIC_TREE = HERE / "public_tree.json"
PUBLIC = ROOT / "build" / "public"
DERIVED = HERE / "derived.json"  # written by public into build/public/aio/, read by restore there
DLLS = ("X3DAudio1_7.dll", "d3d11.dll")
NOTICES = "VinceWaterFix.NOTICES.txt"  # data output (water), shipped under licences/
INI = "VinceAIO.ini"
KNOWN = "known_releases.json"  # aio/known_releases.json, shipped at the zip root
LICENCES = {  # zip member -> repo file
    "licences/VinceFix-LICENSE": "VinceFix/release/github-1.3/LICENSE",
    "licences/Xaudio2-rework-LICENSE": "Xaudio2 rework/LICENSE",
    "licences/minhook-LICENSE.txt": "VinceWaterFix/third_party/minhook/LICENSE.txt",
}
EXTRA_DIRS = (("installer", "installer/"), ("dist", ""))  # aio/<dir> -> zip prefix
DATE_TIME = (1980, 1, 1, 0, 0, 0)
EXTERNAL_ATTR = 0o100644 << 16
LEVEL = 9


class Refused(Exception):
    pass


def sha(b):
    return hashlib.sha256(b).hexdigest()


def held_ids(components):
    return sorted(c["id"] for c in json.loads(Path(components).read_text(encoding="utf-8"))["components"]
                  if c.get("release_held"))


def run_builds(stock):
    """build.py exe, dll, data, options into WORK, strictly in sequence."""
    for cmd in ("exe", "dll", "data", "options"):
        argv = [sys.executable, "-I", str(HERE / "build.py"), cmd] + (["--stock", str(stock)] if cmd != "dll" else [])
        p = subprocess.run(argv + ["--out", str(WORK)], cwd=ROOT, capture_output=True, text=True)
        if p.returncode != 0:
            tail = "\n".join((p.stdout + p.stderr).strip().splitlines()[-5:])
            raise Refused(f"build.py {cmd} exit {p.returncode}:\n{tail}")


def load_own(name):
    return json.loads((WORK / name).read_text(encoding="utf-8"))


def package(stock):
    """{zip member: bytes} of the release from the built WORK tree."""
    if any(p.name == "held" and p.is_dir() for p in WORK.rglob("held")):
        raise Refused("a held/ dir in the build output")
    own_data, own_opt = load_own("ownership-data.json"), load_own("ownership-options.json")
    recs = own_data["outputs"] + own_opt["files"] + [f for c in own_opt["combinations"] for f in c["files"]]
    if any(r["held"] for r in recs):
        raise Refused("a held:true ownership record")
    manifest = {f["path"]: f["sha256"] for f in json.loads(STOCK_MANIFEST.read_text(encoding="utf-8"))["files"]}
    target = manifest["Vince.exe"]
    if load_own("ownership.json")["oracle_sha256"] != target:
        raise Refused("ownership.json oracle_sha256 is not the stock Vince.exe")

    members = {}

    def add(name, data):
        if name in members:
            raise ValueError(f"two files at zip member {name}")
        members[name] = data

    base = {"Vince.exe": WORK / "Vince.exe"}  # always-on game files: game path -> built file
    for r in own_data["outputs"]:
        if r["output"] in DLLS or r["output"] == NOTICES:
            continue
        base[r["output"]] = WORK / r["output"]
    for name in DLLS + (INI,):
        add(name, (WORK / name).read_bytes())
    add(KNOWN, (HERE / KNOWN).read_bytes())
    add(f"licences/{NOTICES}", (WORK / NOTICES).read_bytes())
    for name, rel in LICENCES.items():
        add(name, (ROOT / rel).read_bytes())
    for d, prefix in EXTRA_DIRS:
        if (HERE / d).is_dir():
            for f in sorted((HERE / d).rglob("*")):
                if f.is_file() and "__pycache__" not in f.parts:
                    add(prefix + f.relative_to(HERE / d).as_posix(), f.read_bytes())
    aio_files = {name: sha(data) for name, data in members.items()}

    stock_sha, deltas, sets = {}, {}, []

    def stock_of(game):
        if game not in stock_sha:
            p = Path(stock) / game
            data = p.read_bytes() if p.is_file() else b""
            got = sha(data)
            if manifest.get(game, sha(b"")) != got:
                raise Refused(f"stock {game}: sha256 {got[:16]} is not stock_manifest.json's")
            stock_sha[game] = got if game in manifest else None
        return Path(stock) / game

    for combo in own_opt["combinations"]:
        files = dict(base)
        files.update({f["game_path"]: WORK / f["output"] for f in combo["files"]})
        listed = []
        for game in sorted(files):
            p = stock_of(game)
            src = p.read_bytes() if stock_sha[game] else b""
            new = files[game].read_bytes()
            res = sha(new)
            if res == sha(src):
                raise Refused(f"{game}: built file equals stock")
            if res in deltas:
                if deltas[res][0] != sha(src):
                    raise Refused(f"{game}: result {res[:16]} built from two different sources")
            else:
                deltas[res] = (sha(src), delta.make(src, new))
            listed.append({"game_path": game, "source_sha256": sha(src), "delta": f"deltas/{res}.vvd",
                           "delta_sha256": sha(deltas[res][1]), "result_sha256": res})
        ids, name = option_set(combo["options"])
        sets.append({"set": name, "options": ids, "files": listed})
    if [s["set"] for s in sets] != [option_set([o for i, o in enumerate(OPTIONS) if m >> i & 1])[1]
                                    for m in range(1 << len(OPTIONS))]:
        raise ValueError("ownership-options.json: combinations are not the 8 OPTIONS sets in mask order")
    for res, (_, d) in deltas.items():
        add(f"deltas/{res}.vvd", d)
    man = {"name": PRODUCT, "version": VERSION, "target_exe_sha256": target, "stock_sha256": stock_sha, "sets": sets,
           "aio_files": aio_files, "ini_sha256": aio_files[INI]}
    add("manifest.json", (json.dumps(man, indent=1, sort_keys=True) + "\n").encode("utf-8"))
    return members


def write_zip(path, members):
    with zipfile.ZipFile(path, "w") as z:
        for name in sorted(members):
            info = zipfile.ZipInfo(name, date_time=DATE_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = EXTERNAL_ATTR
            info.create_system = 0
            z.writestr(info, members[name], compresslevel=LEVEL)


def release(stock, components=COMPONENTS, keep_work=False):
    held = held_ids(components)
    if held:
        raise Refused(f"release_held components: {', '.join(held)}")
    stock = Path(stock).resolve()
    if not (stock / "Vince.exe").is_file():
        raise ValueError(f"--stock {stock}: no Vince.exe")
    if any(not (ROOT / d["path"]).is_file() for d in derived_list()):
        restore(stock)
    if OUT.exists():
        shutil.rmtree(OUT)
    WORK.mkdir(parents=True)
    run_builds(stock)
    members = package(stock)
    zpath = OUT / ZIP_NAME
    write_zip(zpath, members)
    if not keep_work:
        shutil.rmtree(WORK)
    return zpath, members


def _under(path, prefix):
    prefix = prefix.rstrip("/")
    return path == prefix or path.startswith(prefix + "/")


def public_files(tree=PUBLIC_TREE, components=COMPONENTS):
    """{public path: repo file} of the public tree; Refused when a held folder or a dev_only path is covered."""
    spec = json.loads(Path(tree).read_text(encoding="utf-8"))
    excl = [e["path"] for e in spec["exclude"]]
    files = {}

    def put(to, src):
        if to in files:
            raise ValueError(f"public_tree.json: two files at {to}")
        if not src.is_file():
            raise ValueError(f"public_tree.json: {src.relative_to(ROOT).as_posix()} is not a file")
        files[to] = src

    for e in spec["entries"]:
        p = e["path"]
        if any(_under(p, x) for x in excl):
            raise ValueError(f"public_tree.json: entry {p} is under an exclude")
        if not p.endswith("/"):
            put(p, ROOT / p)
            continue
        if not (ROOT / p).is_dir():
            raise ValueError(f"public_tree.json: {p} is not a dir")
        for f in sorted((ROOT / p).rglob("*")):
            rel = f.relative_to(ROOT).as_posix()
            if f.is_file() and "__pycache__" not in f.parts and not any(_under(rel, x) for x in excl):
                put(rel, f)
    for m in spec["map"]:
        put(m["to"], ROOT / m["from"])
    comps = json.loads(Path(components).read_text(encoding="utf-8"))["components"]
    barred = [(c["folder"], f"release_held {c['id']}") for c in comps if c.get("release_held")]
    barred += [(d["path"], f"dev_only {c['id']}") for c in comps for d in c.get("dev_only") or []]
    for src in files.values():
        rel = src.relative_to(ROOT).as_posix()
        for path, what in barred:
            if _under(rel, path):
                raise Refused(f"{rel} is covered by {what} ({path})")
        if src.stat().st_size == 0:
            raise Refused(f"{rel}: empty file")
    if DERIVED.relative_to(ROOT).as_posix() in files:
        raise ValueError("public_tree.json: aio/derived.json is written by public, not copied")
    if STOCK_MANIFEST.relative_to(ROOT).as_posix() in files:
        raise ValueError("public_tree.json: aio/stock_manifest.json is written by public, not copied")
    return files


def derived_list():
    """[{path, stock[, encoding]}] of the derived files: public_tree.json `derived` in the repo, aio/derived.json in
    build/public/."""
    if PUBLIC_TREE.is_file():
        return [{k: d[k] for k in ("path", "stock", "encoding") if k in d}
                for d in json.loads(PUBLIC_TREE.read_text(encoding="utf-8")).get("derived", [])]
    if DERIVED.is_file():
        return json.loads(DERIVED.read_text(encoding="utf-8"))["derived"]
    return []


def derived_source(d, stock):
    """Delta source of derived entry d from the stock folder: the stock file's bytes, or with `"encoding": "hex"` their
    lowercase hex text (for files holding stock code bytes as hex strings, e.g. revert data)."""
    data = (Path(stock) / d["stock"]).read_bytes()
    enc = d.get("encoding")
    if enc is None:
        return data
    if enc == "hex":
        return data.hex().encode("ascii")
    raise ValueError(f"derived {d['path']}: unknown encoding {enc!r}")


def derive(target, stock):
    """VVD1 delta stock -> target whose COPYs are exactly the >= 64 B runs aio/gate.py shared_runs finds (dev only).
    shared_runs matches each stock region to one target place only, so target repeats (mip levels) are found by
    re-running it over the still uncovered target segments (>= RUN_MIN) until it finds none."""
    import gate  # aio/gate.py: not in build/public/, only `public` needs it
    runs, segs = [], {0: target}
    while segs:
        new = gate.shared_runs(segs, {"s": stock})
        if not new:
            break
        runs += [(k + p, s, n) for k, p, _, s, n in new]
        cov = bytearray(len(target))
        for to, _, n in runs:
            cov[to:to + n] = b"\1" * n
        segs, i = {}, 0
        while i < len(target):
            j = cov.find(1, i)
            j = len(target) if j < 0 else j
            if j - i >= gate.RUN_MIN:
                segs[i] = target[i:j]
            i = cov.find(0, j)
            i = len(target) if i < 0 else i
    return delta.make_from_runs(stock, target, runs)


def restore(stock):
    """Write each derived <path> from <path>.vvd and the player's stock file; [paths]. Refused on a wrong stock."""
    stock = Path(stock)
    done = []
    for d in derived_list():
        vvd = ROOT / (d["path"] + ".vvd")
        if not vvd.is_file():
            raise Refused(f"{d['path']}.vvd missing")
        try:
            data = delta.apply(derived_source(d, stock), vvd.read_bytes())
        except delta.DeltaError as e:
            raise Refused(f"{d['path']}: stock {d['stock']}: {e}") from None
        (ROOT / d["path"]).write_bytes(data)
        done.append(d["path"])
    return done


def default_stock():
    """env VV_ORACLE, else aio/stock_manifest.json "source" (as aio/gate.py)."""
    return Path(os.environ.get("VV_ORACLE") or json.loads(STOCK_MANIFEST.read_text(encoding="utf-8"))["source"])


def public(tree=PUBLIC_TREE, components=COMPONENTS, stock=None):
    """Write build/public/; (file count, total bytes). Derived files go out as <path>.vvd from `stock`."""
    files = public_files(tree, components)
    derived = json.loads(Path(tree).read_text(encoding="utf-8")).get("derived", [])
    stock = Path(stock) if stock else default_stock()
    out = {to: src.read_bytes() for to, src in files.items()}
    for d in derived:
        p = d["path"]
        if p not in out:
            raise ValueError(f"public_tree.json: derived {p} is not an allowlisted file")
        out[p + ".vvd"] = derive(out.pop(p), derived_source(d, stock))
    if derived:
        out[DERIVED.relative_to(ROOT).as_posix()] = (json.dumps(
            {"derived": [{k: d[k] for k in ("path", "stock", "encoding") if k in d} for d in derived]}, indent=1) + "\n").encode("utf-8")
    if PUBLIC.exists():
        shutil.rmtree(PUBLIC)
    total = 0
    for to, data in sorted(out.items()):
        (PUBLIC / to).parent.mkdir(parents=True, exist_ok=True)
        (PUBLIC / to).write_bytes(data)
        total += len(data)
    man = json.loads(STOCK_MANIFEST.read_text(encoding="utf-8"))
    man.pop("source", None)  # the developer's oracle path
    data = (json.dumps(man, indent=1) + "\n").encode("utf-8")
    (PUBLIC / STOCK_MANIFEST.relative_to(ROOT)).write_bytes(data)
    return len(out) + 1, total + len(data)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    z = sub.add_parser("zip", help=f"build and pack build/release/{ZIP_NAME}")
    z.add_argument("--stock", type=Path, required=True, help="oracle game folder (read only)")
    z.add_argument("--components", type=Path, default=COMPONENTS, help=argparse.SUPPRESS)
    z.add_argument("--keep-work", action="store_true", help="keep build/release/work (aio/gate.py all checks it)")
    pub = sub.add_parser("public", help="write build/public/, the public source tree (aio/public_tree.json)")
    pub.add_argument("--components", type=Path, default=COMPONENTS, help=argparse.SUPPRESS)
    pub.add_argument("--stock", type=Path, help="oracle game folder for the derived .vvd files (read only)")
    rst = sub.add_parser("restore", help="write the derived files from their .vvd and your stock game folder")
    rst.add_argument("--stock", type=Path, required=True, help="your Steam 1.14.2.0 game folder (read only)")
    args = parser.parse_args(argv)
    if args.cmd == "restore":
        try:
            done = restore(args.stock)
        except Refused as e:
            print("refused:", e)
            return 1
        except (OSError, ValueError, KeyError) as e:
            print("error:", e)
            return 2
        print(f"restored {len(done)} files")
        return 0
    if args.cmd == "public":
        try:
            n, size = public(components=args.components, stock=args.stock)
        except Refused as e:
            print("refused:", e)
            return 1
        except (OSError, ValueError, KeyError) as e:
            print("error:", e)
            return 2
        print(f"{PUBLIC.relative_to(ROOT).as_posix()} {n} files, {size} bytes")
        return 0
    try:
        zpath, members = release(args.stock, args.components, args.keep_work)
    except Refused as e:
        print("refused:", e)
        return 1
    except (OSError, ValueError, KeyError) as e:
        print("error:", e)
        return 2
    data = zpath.read_bytes()
    print(f"{zpath.relative_to(ROOT).as_posix()} {len(data)} bytes {sha(data)}")
    print(f"{len(members)} members, {sum(n.startswith('deltas/') for n in members)} deltas")
    return 0


if __name__ == "__main__":
    sys.exit(main())
