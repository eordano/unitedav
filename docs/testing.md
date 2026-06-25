# Testing

All tests use only public-domain or Creative-Commons media (synthetic FFmpeg
`lavfi` clips by default; attribution for any CC clips is in
`tests/media/ATTRIBUTION.md`). Run everything inside the Nix dev shell:

```sh
nix --extra-experimental-features 'nix-command flakes' develop
```

## Native unit + oracle tests
```sh
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build native/build
ctest --test-dir native/build            # all tests
ctest --test-dir native/build -L tier1   # tier-1 unit + oracle suite
ctest --test-dir native/build -L oracle  # pixel/audio correctness vs FFmpeg
```
The oracle suite proves software decode matches an FFmpeg reference, hardware
decode equals software decode, and the colorimetry / sRGB / vertical-flip
contracts hold. Thresholds: `native/tests/oracle/README.md`.

## Memory-safety gate (sanitizers)
```sh
bash tests/run-sanitizers.sh                   # ASan+UBSan+LSan and TSan
UAV_SAN=address bash tests/run-sanitizers.sh   # one config
```
Each config reconfigures a dedicated sanitizer build dir (test GLOBs are
`CONFIGURE_DEPENDS`), rebuilds instrumented, verifies the test target carries
`-fsanitize`, then runs the `tier1` label. A plain `ctest` in a stale or
non-instrumented dir is **not** a memory-safety pass.

## Streaming matrix
A local `mediamtx` (MIT) serves RTSP/RTMP/SRT/HLS/WebRTC for transport round-trips:
```sh
nix --extra-experimental-features 'nix-command flakes' run .#mediamtx
bash tests/run-matrix.sh
```

## CLI probe
```sh
./native/build/uav_probe <url-or-file>   # decode to PPM frames + audio stats
```

## Unity tests
EditMode (API/contract) and PlayMode (open → READY → PLAYING, Info, texture,
audio) tests run headless. Build the native plugin first (see README) so Unity can
load it. Runner project: `tests/unity/`.
