"""Restore the omitted Main Tunnel caption in the English LSBL table.

Builds a separate reversible asset patch. The installed file is read only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import base64
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "subtitle-design"))
sys.path.insert(0, str(ROOT / "tools"))
from format import LSBL
from package_patch import make_manifest

ORIGINAL_SHA256 = "c99099735f607c1419a7e46a679713c4dc86947a4088906070c1924b3ab3580a"
TARGET = 1008
ORIGINAL_TEXT = ("{Two of the items you seek are beyond blocked passageways.}3.2"
                 "{There are monsters you can trick into helping you.}")
THIRD_PHRASE = "But they must be set free."
PATCHED_TEXT = ORIGINAL_TEXT + "3.5{" + THIRD_PHRASE + "}"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def build_patch(original):
    if sha(original) != ORIGINAL_SHA256:
        raise ValueError("unsupported or already changed English subtitle table")
    table = LSBL(original)
    patched = table.replace_text(TARGET, PATCHED_TEXT, expected=ORIGINAL_TEXT)
    return patched, {"format": 1, "target_id": TARGET,
                     "original_sha256": sha(original), "patched_sha256": sha(patched),
                     "third_phrase": THIRD_PHRASE, "third_start_seconds": 6.7,
                     "original_text": ORIGINAL_TEXT, "patched_text": PATCHED_TEXT}


def subtitle_manifest(original, patched):
    manifest = make_manifest(original, patched)
    # UTF-16 has many identical zero bytes between changed bytes. Coalescing small
    # gaps avoids thousands of tiny PowerShell patch operations.
    spans = []
    for part in manifest["ranges"]:
        start = part["offset"]
        end = start + len(base64.b64decode(part["replacement"]))
        if spans and start - spans[-1][1] <= 16:
            spans[-1][1] = end
        else:
            spans.append([start, end])
    manifest["ranges"] = [{"offset": start,
                           "original": base64.b64encode(original[start:end]).decode(),
                           "replacement": base64.b64encode(patched[start:end]).decode()}
                          for start, end in spans]
    rebuilt = bytearray(original) + bytearray(len(patched) - len(original))
    for part in manifest["ranges"]:
        start = part["offset"]
        replacement = base64.b64decode(part["replacement"])
        rebuilt[start:start + len(replacement)] = replacement
    if bytes(rebuilt) != patched:
        raise ValueError("coalesced subtitle manifest failed reconstruction")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="stock English vincestrings.blob (read only)")
    parser.add_argument("--output", type=Path, default=ROOT / "build/subtitle")
    parser.add_argument("--package", type=Path, default=ROOT / "release/VinceSubtitleFix")
    args = parser.parse_args()
    archive = args.package.with_name(args.package.name + ".zip")
    if args.output.exists() or args.package.exists() or archive.exists():
        parser.error("output/package/archive already exists; choose fresh paths")
    original = args.input.read_bytes()
    patched, metadata = build_patch(original)
    manifest = subtitle_manifest(original, patched)
    # Resolve all package inputs before creating files.
    installer = (ROOT / "SubtitleFix.ps1").read_bytes()
    readme = (ROOT / "SUBTITLE_README.md").read_bytes()
    args.output.mkdir(parents=True)
    (args.output / "vincestrings.blob").write_bytes(patched)
    (args.output / "subtitle.json").write_text(json.dumps(metadata, indent=2) + "\n")
    args.package.mkdir(parents=True)
    (args.package / "subtitle-patch.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (args.package / "SubtitleFix.ps1").write_bytes(installer)
    (args.package / "README.md").write_bytes(readme)
    with archive.open("xb") as stream:
        with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED) as z:
            for name in ("SubtitleFix.ps1", "README.md", "subtitle-patch.json"):
                z.write(args.package / name, arcname=args.package.name + "/" + name)
    print(json.dumps({**metadata, "ranges": len(manifest["ranges"]),
                      "package": str(args.package), "archive": str(archive)}, indent=2))


if __name__ == "__main__":
    main()
