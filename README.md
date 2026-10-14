# VinceFix

Smoother frame pacing for **Voodoo Vince Remastered** on PC. VinceFix is a small drop-in DLL that:

- **unpins the game's threads.** The game locks its busy threads to CPUs 0 to 4, so a frame comes out late whenever anything else (Discord, OBS, a driver) runs on one of those CPUs, even while the rest of the processor sits idle.
- **removes an audio stall.** Dozens of times a minute the game asks Windows about your audio device, and each question blocks the main thread for about 1.8 ms. VinceFix answers from memory instead.
- **renders at native resolution.** The game ignores Windows display scaling. At 125 % scaling it renders 2048×1152 and Windows stretches it to 2560×1440; with VinceFix it renders 2560×1440 directly.

It changes no game files, leaves the frame cap and VSync alone, and removes cleanly.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/frame-time-dark.png">
  <img alt="Time between frames along the same route in Midway. Stock game: spiky, worst 24.8 ms on a light load and 42.4 ms with the streaming setup. VinceFix 1.2: tight around 16.7 ms in both, worst 19.1 and 20.4 ms." src="docs/frame-time-light.png">
</picture>

## Results

Same route through the Midway level, about 90 seconds per run, on a fresh install with no other mods. Ryzen 7 5800X3D, RTX 5070 Ti, Windows 11, 2560×1440 at 280 Hz with G-SYNC, Windows scaling 125 %, NVIDIA 60 FPS cap.

| | Stock, light load | **VinceFix, light load** | Stock, streaming | **VinceFix, streaming** |
|---|---:|---:|---:|---:|
| Frames within 0.5 ms of 16.7 ms | 90.3 % | **95.7 %** | 72.6 % | **96.8 %** |
| Frames within 1 ms | 95.1 % | **99.2 %** | 90.6 % | **99.7 %** |
| On-screen intervals within 0.5 ms | 77.0 % | **92.1 %** | 62.6 % | **93.7 %** |
| Frame-time spread (standard deviation) | 0.545 ms | **0.235 ms** | 1.074 ms | **0.222 ms** |
| Slowest 1 % of frames (p99) | 18.46 ms | **17.39 ms** | 19.32 ms | **17.29 ms** |
| Worst frame | 24.8 ms | **19.1 ms** | 42.4 ms | **20.4 ms** |
| Frames over 21 ms | 5 | **0** | 30 | **0** |

*Light load:* no streaming, recording or screen share. *Streaming:* OBS streaming and recording plus a Discord screen share. Measured with PresentMon 2.3.1, seconds 4 to 92 of each run (starting a capture stalls the game once, about 2 seconds in).

With the stock game, the streaming setup makes frame pacing much worse, because its pinned threads have to wait whenever OBS or Discord runs on their CPUs. With VinceFix, streaming costs nothing measurable.

## Install

1. Close the game.
2. Download `VinceFix-1.2.zip` from the [Releases](../../releases) page and put `X3DAudio1_7.dll` from it in the game folder, next to `Vince.exe`. With a default Steam install that is
   `C:\Program Files (x86)\Steam\steamapps\common\Voodoo Vince Remastered`.
   In Steam you can find it with *Manage → Browse local files*.
3. Start the game. A `VinceFix.log` appears in the same folder. You should see:
   - `SetThreadAffinityMask ignored` lines (threads unpinned),
   - `effective awareness 2` (native resolution),
   - a line about XAudio2's `GetDeviceDetails` ending in `now cached`, followed by one `queried XAudio2` line from the game's startup call.
4. If you had ticked Windows' high-DPI override for `Vince.exe` (*Properties → Compatibility → Change high DPI settings*), you can untick it; VinceFix does the same job. The *System* and *System (Enhanced)* overrides block the resolution fix, and the log warns you if one is set.

**Remove it** by deleting `X3DAudio1_7.dll` and `VinceFix.log` from the game folder. Steam's *Verify integrity of game files* doesn't remove it, because it isn't one of the game's files.

### Requirements and compatibility

- Tested with the Steam version 1.14.2.0 of Voodoo Vince Remastered.
- Windows 10 version 1703 or later for the resolution fix.
- The DirectX June 2010 runtime, which the game already needs. VinceFix forwards the game's 3D-audio calls to the real `X3DAudio1_7.dll` from that runtime. If that file is missing, VinceFix shows a message and closes the game rather than running without positional audio.
- Works alongside mods that replace `XINPUT9_1_0.dll` (such as a free-camera mod).

## How it works

VinceFix is named `X3DAudio1_7.dll`, a DirectX audio library the game loads at startup. Windows finds the copy in the game folder first, so VinceFix loads before the game's own code runs. It then:

- **Unpins threads.** It points the game's own import of `kernel32!SetThreadAffinityMask` at a function that does nothing, so Windows schedules every game thread on whichever CPU is free. Only the game's import changes; the graphics driver, overlays and other code in the process keep the real function.
- **Sets per-monitor DPI awareness** before the game creates its window, the same mode as the *Override high DPI scaling behavior: Application* compatibility option.
- **Caches the audio device details.** Each time the game starts a sound that isn't placed in 3D space, it calls XAudio2 2.7's `IXAudio2::GetDeviceDetails(0)`, which lists every audio device through the Windows audio service: about 1.8 ms on the main thread, 40 to 70 times a minute in testing. VinceFix routes the game's `ole32!CoCreateInstance` import through itself. When the game creates its XAudio2 engine, VinceFix points the engine's `GetDeviceDetails` at a cache that the game's own startup call fills. XAudio2 2.7 and the game both fix the output device at startup and never switch, so the cached answer always describes the device the game is playing through. VinceFix also keeps `xaudio2_7.dll` loaded for the life of the game, so Windows can't unload it and drop the change.
- **Forwards 3D audio.** `X3DAudioInitialize` and `X3DAudioCalculate` jump straight to the real DLL in the Windows system folder.

Everything it does is written to `VinceFix.log`.

## Known limits

- Tested on one PC, for about an hour and a half of play.
- The game's two streaming threads used to share one CPU; now they can run on two CPUs at the same time. A hidden timing bug between them could show up as audio or level-streaming glitches. None appeared in testing; if you see any, remove the DLL.
- If you change the Windows default audio device while the game runs, the game keeps playing through the device it started on, as it does without VinceFix. Restart the game to switch.
- VinceFix can't help with stalls caused elsewhere, such as another program holding the GPU or a busy driver taking CPU time from whichever thread is running.

## Build from source

VinceFix is about 430 lines of C and 25 lines of assembly, built freestanding (no C runtime, no Windows SDK, no Visual Studio). You need [LLVM](https://github.com/llvm/llvm-project/releases) (`clang`, `lld-link`, `llvm-dlltool`).

```powershell
pwsh -File build.ps1 -Test
```

This builds `build\X3DAudio1_7.dll` and runs three test hosts:

- loading the DLL at runtime;
- importing it at load time like `Vince.exe`;
- an XAudio2 test against the real `xaudio2_7.dll`, covering the cache, pass-through cases, 8-thread races and the DLL staying loaded.

The build is reproducible (`/Brepro`): the same sources and LLVM version give the same bytes, so you can compare your build with the release DLL. Release 1.2 was built with LLVM 23.1.3.

`install.ps1` copies the build into the game folder (`-GameDir` for a non-default Steam library) and `install.ps1 -Uninstall` removes it. Both refuse to touch an `X3DAudio1_7.dll` that isn't VinceFix.

| Path | Contents |
|---|---|
| `src/vincefix.c` | The DLL: thread unpinning, DPI awareness, audio cache, logging |
| `src/thunks.s` | `X3DAudioInitialize` / `X3DAudioCalculate` forwarding |
| `src/*.def` | Exports and the import libraries (built by `llvm-dlltool`) |
| `tests/` | The three test hosts |

## License

MIT, see [LICENSE](LICENSE). Not affiliated with the game's developer or publisher.
