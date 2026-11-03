"""Fetch pinned official Microsoft headers into build/dependencies.

For the LLVM-only Windows build; unnecessary with a normal installed MSVC SDK.
No system installation or PATH changes. Downloads remain local build inputs and
are never included by package_patch.py. Requires network access and Python3.10+.
"""
import hashlib
from pathlib import Path, PurePosixPath
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEPENDENCIES = ROOT / "build/dependencies"
PACKAGES = (
    ("windows-sdk.nupkg",
     "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp/10.0.28000.2705/microsoft.windows.sdk.cpp.10.0.28000.2705.nupkg",
     "a74ca8f9af98bd61925d9e2932ad98f746306123167b8f0bba99cd9ea9f03807"),
    ("crt-headers.vsix",
     "https://download.visualstudio.microsoft.com/download/pr/c610cd8c-801b-44b8-a80a-82cc382aeb43/852382a9aa73502b7849c1bcadfb603ba7175c4e8b60e6aba03c7de711d4ece5/Microsoft.VC.14.44.17.14.CRT.Headers.base.vsix",
     "852382a9aa73502b7849c1bcadfb603ba7175c4e8b60e6aba03c7de711d4ece5"),
)


def fetch(name, url, expected):
    target = DEPENDENCIES / name
    if not target.exists():
        temporary = target.with_suffix(target.suffix + ".download")
        print(f"Downloading official Microsoft package: {name}")
        with urllib.request.urlopen(url, timeout=60) as response, temporary.open("wb") as output:
            while block := response.read(1024 * 1024):
                output.write(block)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != expected:
            raise ValueError(f"Checksum mismatch: {temporary}")
        temporary.replace(target)
    if hashlib.sha256(target.read_bytes()).hexdigest() != expected:
        raise ValueError(f"Existing package checksum mismatch: {target}")
    return target


def extract(package, prefix, output):
    count = 0
    with zipfile.ZipFile(package) as archive:
        for member in archive.infolist():
            if member.is_dir() or not member.filename.startswith(prefix):
                continue
            relative = PurePosixPath(member.filename[len(prefix):])
            if not relative.parts or relative.is_absolute() or any(part in {"..", "."} or ":" in part for part in relative.parts):
                raise ValueError("Unsafe package member path")
            target = output.joinpath(*relative.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(member))
            count += 1
    if not count:
        raise ValueError("Pinned package layout did not contain expected headers")
    print(f"Extracted {count} header inputs: {output}")


def main():
    DEPENDENCIES.mkdir(parents=True, exist_ok=True)
    sdk, vc = [fetch(*package) for package in PACKAGES]
    sdk_output = DEPENDENCIES / "windows-sdk/c/Include/10.0.28000.0"
    for directory in ("um", "shared", "ucrt"):
        extract(sdk, f"c/Include/10.0.28000.0/{directory}/", sdk_output / directory)
    extract(vc, "Contents/VC/Tools/MSVC/14.44.35207/include/", DEPENDENCIES / "msvc-headers")


if __name__ == "__main__":
    main()
