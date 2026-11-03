# Building the Voodoo Vince Community Patch

Players never build: they download the release zip and run its installer. This page is for anyone who wants to
rebuild that zip from source and check it.

## What you need

- **Your own copy of Voodoo Vince Remastered, version 1.14.2.0, from Steam.** The build reads the stock game files
  and writes every patch as a change against them; no game file is in this repository. Your game folder is only read,
  never written. Check it before you start:
  - `version.txt` in the game folder reads `1 1.14.2.0`;
  - `Vince.exe` has sha256 `f7e08e9bae9656de98b75fffc74bfc5960af8648f0a947ac65343934e5a35489`.

  Every stock file the build touches is checked against `aio/stock_manifest.json`; a modified or other-version game
  folder is refused. Use a clean install (Steam's "Verify integrity of game files" restores one).
- **Windows 10 or 11, 64-bit.** The water shaders compile with Windows' own `d3dcompiler_47.dll`.
- **The pinned toolchain** (`aio/toolchain.py`); other versions build different bytes and the build refuses them:
  - Python 3.15.0, 64-bit, with its bundled zlib-ng 1.3.1 (`zlib.ZLIB_RUNTIME_VERSION` `1.3.1.zlib-ng`);
  - LLVM 23.1.3 (commit `0d261d1ca552c95a8f007e061c787ac7132fbcbc`) installed at `C:/Program Files/LLVM/bin`,
    with that `clang` first on PATH;
  - `C:\Windows\System32\d3dcompiler_47.dll` version 10.0.26100.9711;
  - fxc 10.0.26100.9457 from the Windows 10 SDK (`aio/toolchain.py` `FXC`);
  - the Python packages of `aio/requirements.txt`:
    `PY -m pip install --only-binary=:all: -r aio/requirements.txt`.

  `PY aio/toolchain.py check` prints `ok` or `FAIL` for each pin. Here `PY` is your Python 3.15.0.

## Steps

0. Restore the files derived from game data, from the root of this tree. A few sources (two compiled shaders, a
   shader-patching script, two skin textures) contain bytes of the game's own files, so they ship as `<file>.vvd`
   deltas that only rebuild against your own Steam 1.14.2.0 copy:
   `PY -I aio/release.py restore --stock "<your game folder>"`
   It prints `restored 5 files`; a wrong game version is refused. Step 2 also runs it when a file is missing; run it
   yourself before any other `aio/build.py` command.
1. Fetch the pinned Microsoft header packages the water DLL compiles against (network, once; about 160 MB, checked by
   sha256, never redistributed here):
   `PY VinceWaterFix/tools/fetch_build_headers.py`
2. Build and pack the release, from the root of this tree:
   `PY -I aio/release.py zip --stock "<your game folder>"`
3. The zip is `build/release/VoodooVinceCommunityPatch-<version>.zip`; the command prints its size and sha256. The build is
   deterministic: the same game version and toolchain give the same bytes as the published release.

Scratch files go under `.run/` and `build/`; both are ignored by git.
