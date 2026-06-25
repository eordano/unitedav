# CLAUDE.md — UnitedAV

## What this is
An open-source (Apache-2.0) audio/video plugin for Unity: a C# API in
the `UnitedAV` namespace (`MediaPlayer`, `IMediaControl`, `IMediaInfo`,
`ITextureProducer`, `MediaPlayerEvent`, …) wrapping an FFmpeg-based native
plugin (FFmpeg is LGPL, dynamically linked). It does CPU/software decode + VAAPI
hardware decode today and ships an upstream sender (file/RTP/SRT).

Game code → `MediaPlayer` C# in `unity/` → P/Invoke the C ABI
(`native/include/unitedav.h`) → `libUnitedAV` (C++/FFmpeg) → LGPL FFmpeg. Consumers
touch only the C# surface; the native ABI is internal to this repo.

## Layout
- `native/` — C++ plugin. ABI: `include/unitedav.h` (frozen, cdecl). Decoder:
  `src/decode/decoder.cpp` (FFmpeg demux/decode, worker thread, 3-slot video pool,
  audio ring). Sender: `src/send/`. CLI: `tools/uav_probe.cpp` (`uav_probe`).
- `unity/` — UPM package. C# in `Runtime/UnitedAV/` (namespace `UnitedAV`).
  Compile check: `unity/COMPILE_CHECK.md`.
- `docs/` — `architecture.md`, `testing.md`, `licensing.md`.

## Build & test (Nix; flakes enabled inline via the experimental-features flag)
```sh
# native (Release) + CLI
nix --extra-experimental-features 'nix-command flakes' develop -c bash -c \
  'cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build native/build'
# decode a clip to /tmp PPM frames + audio stats
./native/build/uav_probe <url-or-file>
# memory-safety gate (reconfigures instrumented dirs, asserts targets are
# discovered + carry -fsanitize, runs the tier1 label):
nix --extra-experimental-features 'nix-command flakes' develop -c \
  bash tests/run-sanitizers.sh             # ASan+UBSan+LSan AND TSan
#   UAV_SAN=address (or =thread) for one config.
```
A plain `ctest` in a stale sanitizer dir, or a run in `native/build` with
`UAV_SANITIZE=none`, is **not** a memory-safety pass. `nix build` produces an
LGPL-only FFmpeg (`--disable-gpl`).

## Conventions
- The C ABI is the contract between workstreams — change `unitedav.h` deliberately
  and update both sides + the C# P/Invoke struct layouts together.
- Software decode is the correctness baseline; hardware decode (VAAPI today) must
  match it. Keep the texture contract (`GetTexture()`, `RequiresVerticalFlip()`,
  sRGB) identical across CPU and GPU paths so consumer code stays unchanged.
- LGPL hygiene: never enable FFmpeg `--enable-gpl`/`--enable-nonfree`; link
  dynamically. We only need LGPL/BSD codecs (no GPL/patent encoders).
