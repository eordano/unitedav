# UnitedAV

An open-source (Apache-2.0) audio/video plugin for Unity that wraps
**FFmpeg/libav** (LGPL, dynamically linked) behind a C# API.

UnitedAV does **CPU (software) decode** and **VAAPI hardware decode**
today, and ships an **upstream sender** (capture → encode → send over
file/RTP/SRT). FFmpeg is the only third-party dependency; the rest is C#
and native binding code over it, all open source — no proprietary blobs.

## Design

Game code calls UnitedAV's C# API; the C#↔native boundary is internal. The managed
layer (`unity/Runtime/UnitedAV`) P/Invokes into the native plugin (`native/`),
which uses FFmpeg/libav for demux + decode and hands frames/audio back to Unity.

```
Game code ──► UnitedAV.MediaPlayer  (C# API)
                  │  P/Invoke (C ABI)
                  ▼
              libUnitedAV  (C/C++ native plugin)
                  │
                  ▼
              FFmpeg / libav (LGPL, dynamically linked)
```

## Layout

| Path | What |
|------|------|
| `native/` | C/C++ native plugin (C ABI) + CMake build |
| `unity/`  | C# UPM package — the `UnitedAV` API (Runtime + Editor) |
| `tests/`  | Conformance + media fixtures |
| `docs/architecture.md` | Component design, data flow, threading, memory safety |
| `docs/testing.md`      | How to run native, sanitizer, streaming, and Unity tests |
| `docs/licensing.md`    | License posture + FFmpeg LGPL compliance |

## Build (native)

### With Nix (recommended — reproducible toolchain + FFmpeg)
```sh
nix --extra-experimental-features 'nix-command flakes' develop \
  -c bash -c 'cmake -S native -B native/build -G Ninja && cmake --build native/build'

# or build the plugin as a Nix package
nix --extra-experimental-features 'nix-command flakes' build
```
(Set `experimental-features = nix-command flakes` in `nix.conf` to drop the flag.)

### Without Nix
```sh
cmake -S native -B native/build -DCMAKE_BUILD_TYPE=Release
cmake --build native/build
```
Requires FFmpeg dev libraries (`libavformat`, `libavcodec`, `libavutil`,
`libswscale`, `libswresample`); see `native/CMakeLists.txt`. Without FFmpeg the
build links the C ABI as a stub (useful for wiring the C# layer first).

## Use in Unity

The native plugin is **not committed** — build it and place the artifact under the
matching per-platform folder so Unity loads it. See
`unity/Runtime/Plugins/README.md`. For Linux:
```sh
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build native/build
mkdir -p unity/Runtime/Plugins/Linux/x86_64
cp native/build/libUnitedAV.so unity/Runtime/Plugins/Linux/x86_64/libUnitedAV.so
```

## License

UnitedAV is licensed under **Apache-2.0** (see `LICENSE`). The built plugin
**dynamically links FFmpeg/libav** (LGPL-2.1+); binary redistribution must honor
the LGPL (relinkable, source available). See `NOTICE` and `docs/licensing.md`.
