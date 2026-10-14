# Changelog

## 1.2 (2026-10-14)

First public release.

- **New:** caches XAudio2's audio device details. The game asked Windows about the audio device 40 to 70 times a minute, about 1.8 ms each on the main thread. VinceFix now answers from the game's own startup query. It also keeps `xaudio2_7.dll` loaded so the change can't be dropped.
- Thread unpinning, per-monitor DPI awareness and 3D-audio forwarding, unchanged from 1.1.
- `VinceFix.log` reports the audio cache (the vtable it patched and each real query).

## 1.1 (not released)

- The game's `SetThreadAffinityMask` import becomes a no-op, so its threads are no longer pinned to CPUs 0 to 4.
- Per-monitor DPI awareness before the game creates its window (native resolution with Windows scaling).
- Forwards `X3DAudioInitialize` / `X3DAudioCalculate` to the real DirectX June 2010 DLL; shows a message and exits if it is missing.
