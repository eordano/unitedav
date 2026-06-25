# Architecture

```
Consumer game code
  │  UnitedAV C# API (MediaPlayer / events / …)
  ▼
unity/Runtime/UnitedAV  (managed: texture marshalling, audio routing)
  │  P/Invoke — the native ABI (native/include/unitedav.h)
  ▼
libUnitedAV  (native C ABI / C++ impl)
  │  demux + decode + resample, frame queue, A-V sync
  ▼
FFmpeg / libav  (LGPL, shared libs)
```

## Native plugin internals

```
open(url) ─► libavformat demux ─► packet queue ─┬─► video decode ─► frame queue ─► present
                                                └─► audio decode ─► resample ─► ring buffer ─► Unity audio
```

- **Demux thread:** reads packets, routes per-stream, handles network streaming
  (HLS/DASH via libavformat), EOF/loop.
- **Video decode thread:** decodes to `AVFrame`, converts to RGBA (`sws_scale`) or
  passes NV12, pushes timestamped frames.
- **Audio decode thread:** decodes + `swr_convert` to Unity float-interleaved at
  the engine sample rate, into a lock-free ring buffer.
- **Master clock:** audio clock by default; video presented against it.

## Frame delivery to Unity (CPU path)
- Native keeps the latest presentable frame in a pinned staging buffer; C# polls
  once per render and uploads only on a newer frame.
- Upload via render-thread `CommandBuffer.IssuePluginCustomTextureUpdateV2` +
  native callback (preferred), or `Texture2D.LoadRawTextureData` + `Apply()`.
- **Texture contract:** `GetTexture()` carries correct `width/height`;
  `RequiresVerticalFlip()` and `isDataSRGB` drive the consumer's blit and
  linear↔gamma path. This contract stays identical across CPU and hardware decode.

## Audio delivery to Unity
- A streaming `AudioClip` (PCM read callback) or `OnAudioFilterRead` pulling from
  the native ring buffer, filled by the audio thread.
- A-V sync: video presented against the audio clock (PTS comparison).

## Threading & ownership
- One native player per C# `MediaPlayer`; native owns its decode threads.
- C# does lightweight per-frame polling on the main/render thread.
- ABI getters touch only mutex-protected published buffers, never decode state.

## Memory safety
- All risk lives in the native plugin and the P/Invoke seam. Defenses: a
  lent-pointer slot is excluded from the writer until released (the decode thread
  publishes into a different slot, backing storage is non-reallocating); RAII over
  every FFmpeg object; bounded queues with backpressure; argument validation at the
  ABI boundary.
- The committed gate runs ASan/UBSan/LSan and TSan over the Tier-1 suites on valid
  and hostile inputs (`tests/run-sanitizers.sh`). See `docs/testing.md`.
