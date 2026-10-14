# Changelog

## 1.3 (2026-10-14)

Frame pacing. The 1.2 fixes are unchanged.

- **New: FrameCap.** VinceFix's own frame cap (`FrameCap=60` by default), spacing frame starts exactly 1/60 s apart like the NVIDIA *Max Frame Rate* cap, on any GPU. Turns itself off on displays of 61 Hz or less.
- **New: FrameCap steps aside by itself** while another limiter (a driver or RTSS cap, or VSync) is waiting inside Present, and comes back when that limiter stops; `HoldFlush=auto` stops flushing meanwhile. Leaving a driver cap on is fine; turning it off is still recommended, because VinceFix's own cap measured best. The log says when FrameCap steps aside and when it is back.
- **New: PresentHold.** Holds each finished frame until a set time after it started, so frames reach the screen evenly spaced (adds a little input latency: +0.75 ms at the median with `auto`, +1.6 ms with a fixed 4 ms hold, on the author's PC). It evens out frame spacing; it does not remove stutter. `PresentHold=auto` (default) measures the game's work per frame and picks the hold itself; `1` to `12` is a fixed hold in ms; `0` is off. Turns itself off on displays of 61 Hz or less, and when no frame cap is running.
- **New: HoldFlush.** Hands the frame's graphics work to the GPU before the hold, so it draws during the hold. `HoldFlush=auto` (default) does this only with FrameCap: in a light-load test it raised frames shown within 0.5 ms from 96.9 to 98.1 % with FrameCap, but lowered them from 97.0 to 93.1 % with NVIDIA's cap (one run each). `1` always, `0` never. The log's "ran past the hold" counts frames where the work plus the flush outlasted the hold.
- **New: LiveReload.** `LiveReload=1` applies changes to FrameCap, PresentHold and HoldFlush within about a second of saving `VinceFix.ini`, without restarting the game. Default `0`.
- **New: `VinceFix.ini`** in the release, with the `auto` preset as defaults: `FrameCap=60`, `PresentHold=auto`, `HoldFlush=auto`, `LiveReload=0`. Turning the driver or RTSS frame cap off is recommended (FrameCap steps aside if you leave it on).
- **New log lines** on the first frame: `gpu:`, `display:`, `power:` and `os:`, plus a periodic `present hold stats:` line (hold, late frames, flush time, time inside Present) and `settings reloaded:` after a live reload. They contain no user name, PC name or folder paths.
- **The display is checked again** when the game changes resolution (a new swap chain) or its window moves to another monitor, so the 61 Hz rules follow the display; the log says `display: the game's swap chain changed; display rules checked again` or `display: the game window moved to another monitor; display rules checked again`.
- **Latency in the log:** the `present hold stats:` line also reports the latency of the hold: `frame start to real Present`, `hold wait` and `cap wait`.
- **Results** (one PC, RTX 5070 Ti, 1440p 280 Hz G-SYNC, one run per setup, streaming setup, about 8 minutes of the any% route): frames shown within 0.5 ms of 16.7 ms 69.5 % (stock with the NVIDIA 60 FPS cap) → 98.1 %; frames over 21 ms 36 → 8 (the 8 left are the game's own hitches around loading screens; the mid-level stalls that disappeared are credited to VinceFix as a whole, probably the thread unpinning, not confirmed); slowest 1 % from Present to screen 3.29 → 1.85 ms. AMD and Intel are untested.
- **Trust:** the README now lists what VinceFix does and doesn't do, the Windows functions it uses and how to rebuild the DLL and compare its SHA256.
- `PresentHold`, `FrameCap` and `HoldFlush` change frame timing: speedrunners should check their leaderboard's rules first.

## 1.2 (2026-10-14)

First public release.

- **New:** caches XAudio2's audio device details. The game asked Windows about the audio device 40 to 70 times a minute, about 1.8 ms each on the main thread. VinceFix now answers from the game's own startup query. It also keeps `xaudio2_7.dll` loaded so the change can't be dropped.
- Thread unpinning, per-monitor DPI awareness and 3D-audio forwarding, unchanged from 1.1.
- `VinceFix.log` reports the audio cache (the vtable it patched and each real query).

## 1.1 (not released)

- The game's `SetThreadAffinityMask` import becomes a no-op, so its threads are no longer pinned to CPUs 0 to 4.
- Per-monitor DPI awareness before the game creates its window (native resolution with Windows scaling).
- Forwards `X3DAudioInitialize` / `X3DAudioCalculate` to the real DirectX June 2010 DLL; shows a message and exits if it is missing.
