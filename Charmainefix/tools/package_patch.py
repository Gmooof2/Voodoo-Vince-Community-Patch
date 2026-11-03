"""Package verified build differences, without distributing the game executable.

The original executable is read only. Existing output folders/archives are never
overwritten. The PowerShell installer reconstructs the candidate and verifies its
full SHA256 before replacing any executable.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def make_manifest(original: bytes, patched: bytes):
    """Return format-1 sorted, disjoint ranges, including one EOF append range."""
    if not original or len(patched) < len(original):
        raise ValueError("empty originals and shrinking patches are unsupported")
    if original == patched:
        raise ValueError("candidate is identical to the original")
    if len(patched) > 536870912:
        raise ValueError("candidate exceeds the installer's 512MB limit")
    ranges = []

    def add(start, end, old):
        ranges.append({"offset": start,
                       "original": base64.b64encode(old).decode("ascii"),
                       "replacement": base64.b64encode(patched[start:end]).decode("ascii")})

    start = None
    for offset, (old_byte, new_byte) in enumerate(zip(original, patched)):
        if old_byte != new_byte and start is None:
            start = offset
        elif old_byte == new_byte and start is not None:
            add(start, offset, original[start:offset])
            start = None
    if start is not None:
        add(start, len(original), original[start:])
    if len(patched) > len(original):
        add(len(original), len(patched), b"")
    manifest = {"format": 1, "originalSha256": sha(original), "patchedSha256": sha(patched),
                "originalLength": len(original), "patchedLength": len(patched), "ranges": ranges}
    # Exercise exactly the fixed-buffer reconstruction used by the installer.
    rebuilt = bytearray(original) + bytearray(len(patched) - len(original))
    for part in ranges:
        offset = part["offset"]
        replacement = base64.b64decode(part["replacement"])
        rebuilt[offset:offset + len(replacement)] = replacement
    if bytes(rebuilt) != patched:
        raise ValueError("manifest failed exact reconstruction")
    if len(json.dumps(manifest, indent=2).encode("utf-8")) > 16777216:
        raise ValueError("manifest exceeds the installer's 16MB metadata limit")
    return manifest


def package(original_path, patched_path, output, archive):
    original_path, patched_path = Path(original_path).resolve(), Path(patched_path).resolve()
    output, archive = Path(output).resolve(), Path(archive).resolve()
    if output.exists() or archive.exists():
        raise ValueError("package folder/archive already exists; choose fresh output paths")
    if output == archive or output in archive.parents:
        raise ValueError("archive must be outside the package folder")
    original, patched = original_path.read_bytes(), patched_path.read_bytes()
    metadata_path = patched_path.with_suffix(".narration.json")
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    if (metadata.get("format") != 1 or metadata.get("original_sha256") != sha(original)
            or metadata.get("patched_sha256") != sha(patched)):
        raise ValueError("build metadata does not match the supplied executable hashes")
    manifest = make_manifest(original, patched)
    # Resolve and read all package inputs before creating output.
    installer = (ROOT / "NarrationFix.ps1").read_bytes()
    readme = (ROOT / "README.md").read_bytes()
    output.parent.mkdir(parents=True, exist_ok=True)
    archive.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".narration-package-", dir=output.parent))
    (staging / "patch.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (staging / "NarrationFix.ps1").write_bytes(installer)
    (staging / "README.md").write_bytes(readme)
    # Publish only these three allowlisted assets; no executable or backup enters
    # the archive. Exclusive creation also protects an existing archive.
    with archive.open("xb") as stream:
        with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
            for name in ("NarrationFix.ps1", "README.md", "patch.json"):
                bundle.write(staging / name, arcname=f"{output.name}/{name}")
    staging.rename(output)
    return {"package": str(output), "archive": str(archive),
            "originalSha256": manifest["originalSha256"],
            "patchedSha256": manifest["patchedSha256"], "ranges": len(manifest["ranges"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original", type=Path, required=True)
    parser.add_argument("--patched", type=Path, default=ROOT / "build/Vince.exe")
    parser.add_argument("--output", type=Path, default=ROOT / "release/VinceNarrationFix")
    parser.add_argument("--zip", dest="archive", type=Path,
                        help="archive path (default: sibling of package folder, with .zip suffix)")
    args = parser.parse_args()
    archive = args.archive or args.output.parent / (args.output.name + ".zip")
    print(json.dumps(package(args.original, args.patched, args.output, archive), indent=2))


if __name__ == "__main__":
    main()
