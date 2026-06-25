# UnitedAV — Tier-1 pixel/audio oracle (`uav_oracle`)

The oracle is the **correctness GATE** layered on top of the
`tests/run-matrix.sh` smoke harness. They share the same `tests/media/out`
inputs and the same `UAV_HWDECODE` toggle, so **no media is regenerated**:

| Harness              | Proves                                              |
|----------------------|----------------------------------------------------|
| `tests/run-matrix.sh`| "it decodes & is non-silent" (smoke)               |
| `uav_oracle`         | "it decodes **correctly vs an FFmpeg reference**, **HW == SW**, and the **colorimetry / orientation / sRGB** contracts hold" |

It drives the **frozen** public C ABI (`uav_create`/`open`/`play`/
`acquire_frame`/`release_frame`/`read_audio`/`get_info`) against the **shipped
`libUnitedAV`** for the decode-under-test, and independently decodes the same
clip with the same LGPL libav\* the plugin links (`oracle_ref.cpp`) for the
reference. The metric math (`oracle_metrics.cpp`) has **zero
third-party deps** (no OpenCV / libpng / any GPL or image lib), so it is
unit-testable on synthetic inputs without media or FFmpeg.

## Files

| File | Role |
|------|------|
| `uav_oracle.cpp`         | the tool: `main()`, the three modes, the report. **Only `main()` in the GLOB.** |
| `oracle_ref.{hpp,cpp}`   | FFmpeg reference extractor (the only TU that links libav\*); SW-only, top-down RGBA, byte-identical to `decoder.cpp`. |
| `oracle_metrics.{hpp,cpp}` | pure PSNR / SSIM / Pearson / mean\|d\| / RMS math (no deps). |
| `oracle_metrics_test.cpp`  | metric UNIT checks; **no `main()`** — exposes `oracle_metrics_selftest()`, run by `uav_oracle --selftest`. |
| `run_oracle.cmake`       | optional ctest/`-P` wrapper that registers one case per clip per mode (see "Per-clip ctest"). |

> All `tests/oracle/*.cpp` GLOB into the SINGLE `uav_oracle` binary (see the
> scaffold in `native/CMakeLists.txt`). That is why `oracle_metrics_test.cpp`
> must not define `main()`. After adding a new `.cpp` you must **re-run cmake**
> (the GLOB is `CONFIGURE_DEPENDS`; a bare `cmake --build` won't pick it up).

## Modes & CLI

```
uav_oracle --selftest                         # metric unit checks (no media)
uav_oracle <clip> --mode sw-vs-ref  [opts]    # plugin SW decode vs FFmpeg reference
uav_oracle <clip> --mode hw-vs-sw   [opts]    # plugin HW decode vs plugin SW decode
uav_oracle <clip> --mode contract   [opts]    # colorimetry / sRGB / vertical-flip
opts: --frames N --min-psnr X --min-ssim X --min-acorr X --max-hw-meandiff X
```

Output is one `KEY=VALUE` line per metric and a final `PASS=1` / `PASS=0`.
Exit codes: **0 = PASS, 1 = FAIL, 2 = open/usage error, 77 = ctest-SKIP**
(HW unavailable / nothing to compare).

## Thresholds and WHY

| Metric | Default | Rationale |
|--------|---------|-----------|
| SW-vs-ref luma **SSIM** | **>= 0.98** | The plugin and the reference share ONE benign `sws_scale` **BILINEAR** pass (`AV_PIX_FMT_RGBA`, top-down) — identical pipelines — so structural similarity is near-perfect; 0.98 leaves headroom only for codec-internal rounding, not a real defect. |
| SW-vs-ref **PSNR** | **>= 35 dB** | Same shared BILINEAR pass on both sides; 35 dB flags any visible deviation (wrong colour matrix, swapped channels, half-resolution chroma) while tolerating ±1 LSB YUV→RGB rounding. |
| Audio per-channel **Pearson** | **>= 0.99** | A small lag search (±`rate/50` ≈ 20 ms) absorbs decoder priming/latency; below 0.99 means a wrong sample format, channel swap, or resample artefact. |
| Audio **RMS ratio** (ABI/ref) | **0.5 .. 2.0** | A silent or over-attenuated path (e.g. volume/mute applied where it should not be) falls outside this band and FAILS, so "all zeros" cannot pass the correlation check by accident. |
| HW-vs-SW **mean\|d\|** (RGB) | **<= 6 / 255** | Reuses the documented `uav_gpu_probe` 6.0 threshold — VAAPI's YUV→RGB differs from swscale's by a few LSB; 6/255 is generous for that and tight for a real colour bug. |
| HW-vs-SW **SSIM** | **>= 0.98** | Same structural-equality bar as SW-vs-ref. |

**Ground-truth colorimetry** for the SD `testsrc2` matrix (320×240, 8-bit):
**BT.601 / limited (`mpeg`) range** — the `contract` mode asserts this so a
decoder that silently normalises colorimetry (e.g. forces full-range or BT.709)
fails. For 10-bit / HDR clips (`mkv__av1_10__opus`) the values are **recorded,
not gated** (`colorimetry_bt601_limited_ok=-1`).

**sRGB:** the decoded transfer must be sRGB / BT.709-curve consistent (NOT
linear-light), matching the consumer's `EnsureTexture(linear:false)` /
`Texture.isDataSRGB` simple-flip-blit branch. `testsrc2` leaves TRC
*unspecified* (gamma-domain, the sRGB-consistent default), which passes; an
explicit LINEAR transfer FAILS.

**RequiresVerticalFlip:** the reference is decoded TOP-DOWN (row 0 = top),
exactly like `decoder.cpp`. The oracle compares the ABI frame top-down (high
SSIM) vs vertically-flipped (low SSIM) against that reference. Top-down winning
positively proves the native buffer is top-down ⇒
`ITextureProducer.RequiresVerticalFlip() == true` is the correct managed
contract (the C# bool is asserted by the committed Unity EditMode test).

## HW-vs-SW SKIP policy

Matches `run-matrix.sh`'s `hwdecode_for()`: **VP8/VP9 get only `sw-vs-ref` +
`contract`** (the documented VAAPI VP8/VP9 decode gap). For HW-decodable codecs
(H.264 / HEVC / AV1), if HW init is unavailable the decoder silently falls back
to SW; the oracle DETECTS the byte-identical fallback (`mean|d| == 0`) and
returns **77 (ctest SKIP)** rather than FAIL — HW absent is a skip, not a
failure.

## Running

```sh
# build once (shared with the rest of the native suite — disk-conscious)
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build native/build

# the always-runnable metric leg + binary discovery (registered by the scaffold)
ctest --test-dir native/build -R oracle_selftest

# the full per-clip pixel/audio gate (run_oracle.cmake is wired into the scaffold)
ctest --test-dir native/build -L oracle      # oracle.swref.* / oracle.contract.* / oracle.hwsw.*
# or directly, one process per clip:
./native/build/uav_oracle tests/media/out/webm__vp9__opus.webm --mode sw-vs-ref
```

The same binary builds under the sanitizer dirs (`native/build-asan`,
`native/build-tsan`) with no extra wiring (inherits `UAV_SANITIZE`) — but only
once that dir is **(re)configured with the oracle `.cpp` files present**, because
the GLOB is `CONFIGURE_DEPENDS` (evaluated at configure time). A sanitizer dir
configured before these files existed has no `uav_oracle` target at all, so an
ASan/TSan pass requires the reconfigure. Use the committed, self-checking gate
`tests/run-sanitizers.sh` (it reconfigures, rebuilds, asserts the target exists,
then runs ctest under ASan/UBSan/LSan — or TSan via `UAV_SAN=thread`) to get a
real ASan/LSan + TSan pass over the oracle.
