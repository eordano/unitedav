# UnitedAV — Tier-2 libFuzzer targets

Byte-level fuzzing of the **frozen** public C ABI (`native/include/unitedav.h`,
`native/include/unitedav_send.h`) — the exact entry points the C# P/Invoke layer
calls. The oracle is the **toolchain**, not a return code: no ASan/UBSan finding,
no LSan report at exit, no hang. A malformed input legitimately fails to open.

## Targets (one libFuzzer executable per `tests/fuzz/*.cpp`)

| source | ctest case | what it covers |
|---|---|---|
| `fuzz_open.cpp` | `fuzz_fuzz_open` | `uav_open` + the **decode loop** (play / acquire / release / read_audio / seek) once a seed opens a real stream |
| `fuzz_decode_url.cpp` | `fuzz_fuzz_decode_url` | the demux **open + stream-probe** path on arbitrary container bytes (open-only) |
| `fuzz_url.cpp` | `fuzz_fuzz_url` | the **url / scheme / protocol-whitelist** string surface of `uav_open`: fuzz bytes go **directly** as the `url` arg (no temp file), driving avformat scheme parse + the `.sdp` / `rtp://` / `rtsp://` whitelist predicate + the reconnect options. Offline by construction — every url shape resolves to a missing local file or a closed loopback port, never the network |
| `fuzz_send_config.cpp` | `fuzz_fuzz_send_config` | the upstream **sender**: `uav_send_open` + `push_video` / `push_audio` from a fuzz-derived `UAVSendConfig` |

All targets are **offline by construction** (local temp file / `file://` only) and
unlink their temp files each iteration (disk-conscious). They are opt-in
(`-DUAV_FUZZ=ON`) and **clang-only** (libFuzzer is part of the clang toolchain —
no GoogleTest/Catch2/GPL). On a build without FFmpeg dev libs the ABI is stubbed
and the targets degrade to a no-crash create/open→`UNSUPPORTED`/teardown loop; the
sangate driver records that as SKIP(no-ffmpeg), not a pass.

## Why the seed corpus is load-bearing

`run_decode_loop` in `fuzz_open.cpp` runs **only** after `uav_open()==UAV_OK`,
which needs real container magic + a decodable stream. Starting from an **empty**
corpus, libFuzzer will not synthesize a valid WebM/MP4/MKV inside the 10s smoke
run, so only the avformat probe-and-fail branch would execute and the decode
pipeline this target exists to cover would be dead code in CI. Likewise
`fuzz_send_config` needs config bytes that select a real encoder before the push
path is reached.

The committed seeds under `corpus/<target>/` fix that: the ctest `add_test`
passes `corpus/<target>/` as libFuzzer's **last positional arg**, so libFuzzer
both **seeds** from these inputs (reaching the decode/encode pipeline on the very
first exec) and **persists** newly-discovered inputs back into the directory, so
successive smoke runs compound coverage.

## Corpus layout

Produced by [`gen-corpus.sh`](./gen-corpus.sh) from the synthetic clips in
`tests/media/out` (public-domain `testsrc2`/`sine`; **no GPL tools** — derived
with `dd`/`printf`/`cp` only):

- `seed-trunc{1024,4096}-<clip>.bin` — header-only truncations (probe + partial
  decode). **Committed.**
- `seed-flip-<clip>.bin` — a 4K header prefix with one byte flipped (error
  recovery). **Committed.**
- `seed-cfg-*.bin` (`fuzz_send_config` only) — small `UAVSendConfig`-shaped LE
  u32 blobs that open a real encoder. **Committed.**
- `seed-url-*.bin` (`fuzz_url` only) — tiny ASCII seeds whose first byte selects
  each offline url shape (bare path / `file:` / `.sdp` / `rtp://` / `rtsp://`) so
  the scheme-parse + protocol_whitelist predicate is reached from seed 0.
  **Committed.**
- `clip-<clip>.bin` — full-clip copies that drive deep decode. **Gitignored**
  (large; mirrors the `tests/media/out` policy — regenerated, never committed).

The committed `seed-*` are tiny and guarantee the decode loop is reached even on
a fresh checkout; run `gen-corpus.sh` once locally to add the full-clip depth.

## Build & run (inside the dev shell)

```sh
# (optional) regenerate media + corpus
bash tests/media/gen.sh
bash native/tests/fuzz/gen-corpus.sh

# configure + build the fuzz targets (clang)
cmake -S native -B native/build-fuzz -G Ninja \
  -DUAV_BUILD_TESTS=ON -DUAV_FUZZ=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build native/build-fuzz

# bounded CI smoke (10s/target; seeds + persists into corpus/<target>/)
ctest --test-dir native/build-fuzz -L fuzz

# a single target, or a longer ad-hoc campaign:
ctest --test-dir native/build-fuzz -R fuzz_fuzz_open
./native/build-fuzz/fuzz_open -max_total_time=300 native/tests/fuzz/corpus/fuzz_open
```

The committed gate driver `tests/run-sangate.sh` runs the `ctest -L fuzz` leg and
warns+SKIPs (rather than claiming a pass) when the build does not link FFmpeg.
