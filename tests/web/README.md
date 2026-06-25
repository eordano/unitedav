# UnitedAV — WebGL backend

A Unity **WebGL** build cannot load the native FFmpeg plugin (no native P/Invoke in
the browser sandbox). So on WebGL, UnitedAV uses the **browser's own video stack**:
an `HTMLVideoElement` decodes the clip (hardware-accelerated where the browser
supports the codec) and each frame is uploaded into Unity's WebGL texture with
`texImage2D(..., videoElement)` — behind the same `IMediaControl` / `IMediaInfo` /
`ITextureProducer` surface, so consumer code is unchanged.

## Pieces
- `unity/Runtime/Plugins/WebGL/UnitedAV.jslib` — the browser bridge (create/play/
  pause/seek/state/dimensions + `UAV_Web_UploadTexture` into a Unity GL texture).
- `unity/Runtime/UnitedAV/Internal/UnitedAVWebGL.cs` — `[DllImport("__Internal")]`
  declarations.
- `unity/Runtime/UnitedAV/WebGLMediaSource.cs` — C# source implementing the player
  interfaces; call `Tick()` once per frame. Select it under
  `#if UNITY_WEBGL && !UNITY_EDITOR`.

## Verify the upload path (no Unity needed)
```sh
bash tests/web/run-webgl.sh
```
Loads a VP9 webm into a `<video>`, uploads to a WebGL2 texture, reads it back, and
checks it against (1) a 2D-canvas `drawImage` of the same frame and (2) an ffmpeg
oracle, in headless Chromium. Expected:
```
UAVRESULT PASS webgl_vs_2d_mean=0.000 max=0 flip=false oracle_mean~0.7 ...
WEB-PASS: WebGL video->texture verified
```
VP9 is used because Chromium's H.264 support is build-dependent; the runtime path
plays whatever the user's browser supports.

## WebGPU backend

A Unity **WebGPU** player build uses the same browser decode half (HTMLVideoElement)
but produces a `GPUTexture` instead of a GL texture. The Unity plumbing:
- `unity/Runtime/Plugins/WebGL/UnitedAV.jslib` — `UAV_Web_UploadTextureWGPU` beside the
  GL upload (shares the decode half). The GPUTexture/GPUDevice emscripten-registry
  names remain a **runtime-verify TODO** against a real Unity WebGPU build; the
  conversion shader/math is the verified one below.
- `unity/Runtime/UnitedAV/Internal/UnitedAVWebGL.cs` — the `[DllImport]` declaration.
- `unity/Runtime/UnitedAV/WebGLMediaSource.cs` — `Tick()` dispatches on
  `SystemInfo.graphicsDeviceType == GraphicsDeviceType.WebGPU`.

### Color: do the YUV->RGB ourselves (WebCodecs)
Chromium's `copyExternalImageToTexture` / `importExternalTexture` apply a YUV->RGB
conversion that **ignores the clip's tagged matrix** (a BT.601-vs-709 drift), so they
don't even match the browser's own `<video>` presentation. The fix — and the default
path — is to do the conversion ourselves, exactly like the native backends:

> **mode=webcodecs (default):** `VideoFrame.copyTo` -> raw Y/U/V planes -> upload as
> r8 textures -> a WGSL shader applies the clip's signalled matrix+range
> (`coeffs_for()`/`make_matrix()`). The clip's matrix comes from
> `VideoFrame.colorSpace` (e.g. `smpte170m` => BT.601). Cost: a per-frame plane
> `copyTo` — WebGPU exposes **no** zero-copy access to a frame's raw YUV planes (the
> only zero-copy import, `importExternalTexture`, hides the YUV behind the browser's
> own conversion). So exact color and full no-copy can't both hold today; correct
> color wins.

### Verify (no Unity needed)
```sh
bash tests/web/run-webgpu.sh            # mode=webcodecs (correct color), default
UAV_WEB_MODE=external bash tests/web/run-webgpu.sh   # zero-copy; browser conversion
```
Loads a VP9 webm, builds the RGBA `GPUTexture`, reads it back (`copyTextureToBuffer`
-> `mapAsync`, 256-row-aligned then de-padded), and checks it against (1) the 2D-canvas
`drawImage` of the same frame and (2) an ffmpeg oracle. Capture uses the first frame
without playing so all three reads are the SAME frame.

Host flags:
- **Linux** (fallback): chromium on PATH; a real Dawn adapter needs `--use-angle=vulkan`
  (SwiftShader/`--disable-gpu` dies at `mapAsync`). Adapter: `intel/gen-12lp`.
- **SHADE (mac, canonical)** — Apple Silicon, Metal-3:
  ```sh
  UAV_WEB_BROWSER="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  UAV_FFMPEG="nix run nixpkgs#ffmpeg --" bash tests/web/run-webgpu.sh
  ```
  (homebrew chromium on SHADE is Killed:9 — Google Chrome.app is required.)

### Verified results — parity achieved
```
# mode=webcodecs (correct, default):
SHADE  Chrome 149  apple/metal-3 : UAVRESULT PASS  webgpu_vs_2d_mean=0.387 oracle_mean=2.746
Linux  chromium    intel/gen-12lp: UAVRESULT PASS  webgpu_vs_2d_mean=0.387 oracle_mean=3.108
# for contrast, the browser's built-in conversion (FAILS the <2.0 gate):
Linux  mode=copy     : webgpu_vs_2d_mean=15.3   (BT.601-decoded-as-709)
Linux  mode=external : webgpu_vs_2d_mean=15.3   (zero-copy; same browser conversion)
Linux  mode=external + in-shader affine correction : 5.5   (better, still not parity)
```
The webcodecs path matches the browser presentation to **mean|d|=0.387** and ffmpeg
to ~2.7-3.1 (the same cross-decoder gap the native backends show vs swscale).
`tests/web/webgpu_diag.html` is the diagnostic that characterized the mismatch
(logs `VideoFrame.colorSpace`, the affine fit, and all paths side by side).

## Mic + speaker test
`tests/web/av_io_test.html` + `run-av-io.sh`: a WebAudio 440 Hz tone to the speaker
(output level meter) + `getUserMedia` mic capture (live input meter; PASS when signal
detected). Auto-verifiable headless — `run-av-io.sh` runs Chrome with
`--use-fake-device-for-media-stream --use-fake-ui-for-media-stream` (fake mic emits a
tone, permission auto-granted) → deterministic PASS (no macOS TCC needed). Verified:
shade (Chrome) speaker_rms≈0.11 / mic_rms≈0.34, Linux (chromium) likewise. Live, it
shows the real Mac mic/speaker (approve the mic prompt). It's the 4th `record-demo.sh`
segment.

## Serving the pages (don't use file://)
WebCodecs / WebGPU / `getUserMedia` need a real http origin + secure context. A
`file://` URL is a unique origin (video load is CORS-blocked) and not secure, so the
pages won't work opened as files. Serve over `127.0.0.1` (a secure-context exception):
```sh
bash tests/web/serve.sh                 # prints the player / probe / mic URLs
UAV_OPEN=1 bash tests/web/serve.sh      # also opens the WebGPU player
```

## Notes / limits
- Audio plays through the `HTMLVideoElement` itself (no PCM tap), so `HasAudio()`
  reports false and there is no audio-sample callback on WebGL.
- The clip must be reachable same-origin (or CORS-enabled) or the canvas taints and
  readback/sampling is blocked.
- Codec support and HW decode are the browser's; there is no FFmpeg in the build.
