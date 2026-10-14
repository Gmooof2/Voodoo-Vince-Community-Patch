# Building VinceFix and checking the official DLL

VinceFix is one small DLL (`X3DAudio1_7.dll`) built from the C and assembly files in this folder. The build is
reproducible: the same sources and the same compiler always give a byte-identical DLL, so anyone can rebuild it and
check that the published DLL was built from this code and nothing else.

## What you need

- Windows 10 or 11.
- **LLVM 23.1.3** for Windows (the official release installer from the LLVM project's GitHub page), installed in
  `C:\Program Files\LLVM`. Another version builds a working DLL too, but its bytes may differ; `build.ps1` warns and
  names both versions when the installed one is not 23.1.3.
- PowerShell 7 (`pwsh`).

No Visual Studio, Windows SDK or C runtime is needed: the DLL is built freestanding, and the import libraries are made
from the `.def` files.

## Build

```
pwsh -File build.ps1
```

The DLL lands in `build\X3DAudio1_7.dll`. Each build also:

- builds the DLL a second time in `build\repro-check\` and stops with an error unless both copies have the same
  SHA256 (it prints the hash);
- reads the DLL's import table and stops with an error if the DLL imports any Windows function that
  `imports-allowed.txt` does not list, or no longer imports one it lists. That file says why each function is used,
  and also lists the few functions VinceFix looks up while running (`GetProcAddress`).

## Compare with the official DLL

```
Get-FileHash build\X3DAudio1_7.dll
Get-FileHash "<game folder>\X3DAudio1_7.dll"
```

Both must show the SHA256 published with the release. The source files that go into the DLL are `dllmain.c`,
`common.c`, `hooks.c`, `audio.c`, `settings.c`, `present.c`, `setup_info.c`, `vincefix.h`, `thunks.s` and the
`.def` files; `vincefix.h` lists what each one does.

## Tests

`pwsh -File build.ps1 -Test` also runs four test programs against the new DLL (about 3 minutes; it keeps one CPU core
busy, and its timing checks can fail on a busy PC: rerun them on a quiet one). The XAudio2 test needs the DirectX June
2010 runtime, as the game does.
