# UnitedAV — End-to-End sample + headless integration battery

This sample is two things in one scene:

1. **A demo that USES the package** — `MediaPlayerDisplay` binds a
   `MediaPlayer`'s `TextureProducer` onto a UI `RawImage` (and optionally a 3D
   `Renderer`), honoring `ITextureProducer.RequiresVerticalFlip()` exactly the way
   a consumer's video-texture binding should.
2. **A self-testing standalone Player** — `IntegrationBattery` drives a real
   `MediaPlayer` through the public UnitedAV surface, writes a JSON report, and sets
   the process exit code. Build the scene into a Player, launch it, and judge it by
   its exit status + report file — no test runner required.

Everything here uses **only the public `UnitedAV` C# surface** (`MediaPlayer`,
`IMediaControl`, `IMediaInfo`, `ITextureProducer`, `MediaPlayerEvent`) — the same
API consumers depend on.

## Files

| File | Role |
|------|------|
| `EndToEnd.unity` | Scene: Camera, Canvas+RawImage (`VideoSurface`), `EventSystem`, and a `UnitedAV E2E` object carrying `MediaPlayer` + `IntegrationBattery` + `MediaPlayerDisplay`. |
| `IntegrationBattery.cs` | The headless battery: runs the cases, writes JSON, sets exit code. |
| `MediaPlayerDisplay.cs` | Presents the decoded texture onto a `RawImage`/`Renderer` with the V-flip contract. |
| `BatteryResult.cs` | `[Serializable]` DTOs (`BatteryReport`, `CaseResult`, `CaseStatus`) for the JSON report. |

## What the battery checks

In order (`open` gates the media-dependent cases; the rest are managed-only):

| id | Checks |
|----|--------|
| `open` | `OpenMedia(...)` reaches READY with sane `Info` width/height/fps. |
| `play` | `Control.Play()`: `GetCurrentTime()` advances, `IsPlaying()` true. |
| `pause` | `Control.Pause()`: `IsPaused()` true and the clock freezes. |
| `seek` | `Control.Seek(t)`: `GetCurrentTime()` lands near `t`. |
| `texture_nontrivial` | The decoded frame is not a single flat color (pixel readback). |
| `events` | A `MediaPlayerEvent` listener observes ≥1 event; `HasListeners()` true. |
| `loop` | `SetLooping`/`IsLooping` managed round-trip (the ABI is set-only). |
| `volume` | `AudioVolume` clamps to `[0,1]` and round-trips. |
| `mute` | Silence via `AudioVolume = 0`, then restore. **There is no public `Mute` API** on the UnitedAV C# surface (the native `uav_set_muted` is not surfaced through C#), so `AudioVolume = 0` is the public mute path. |
| `hw_backend` | With `UAV_HWDECODE=auto` + an **H.264** fixture, the native plugin logs `[uav] hardware decode enabled: <vaapi\|videotoolbox\|cuda>`. |

### Why the HW probe insists on H.264

Hardware decode only engages on H.264 in this build. VP9/VP8/AV1 have no
VideoToolbox / (usefully complete) NVDEC / VAAPI path here and **silently fall
back to software** — so probing HW with a VP9 clip would look like a SW failure
when it is just an unsupported codec. The battery therefore resolves a dedicated
H.264 fixture (`mp4__h264__aac.mp4`, `mov__h264__aac.mov`, or
`mpegts__h264__mp3.ts`) for this case.

The backend name is **not exposed through the C# ABI** — the native plugin emits
the `[uav] hardware decode enabled:` line on FFmpeg's log (stderr), which a
standalone Player routes to its log file (`Application.consoleLogPath`, i.e.
`Player.log`). The battery scans that log after decoding the H.264 fixture and
records the backend token it finds. A run leg can grep the same file:

```sh
grep -m1 'hardware decode enabled' "<Player.log>"
```

## Graceful, never a fake pass

Each case is one of `Pass` / `Fail` / `Skip`:

- **Pass** — the contract held.
- **Fail** — a real defect (exit code `1`).
- **Skip** — the case could not be exercised here (no native plugin, no fixture,
  no HW backend, headless GPU readback unavailable). A skip carries a `blocker`
  (why) and a `remediation` (how to make it run) and is **never** counted as a
  pass.

`overallPass` is true only when **every** case passed (skips do not count as
green).

## Exit codes (standalone Player)

| Code | Meaning |
|------|---------|
| `0` | All cases passed. |
| `1` | At least one real failure (a defect). |
| `2` | No failures, but at least one skip (blocked / un-exercised). |

`IntegrationBattery.quitOnFinish` calls `Application.Quit(code)`, which sets the
process exit code on a standalone Player. In the Editor it is a no-op for the
exit code, so set `quitOnFinish = false` when iterating in-editor.

## Environment variables

| Var | Effect |
|-----|--------|
| `UAV_HWDECODE=auto` | Request hardware decode (required for `hw_backend` to attempt HW). |
| `UAV_TEST_MEDIA_DIR` | Folder to resolve fixtures from. Falls back to a repo-relative `tests/media/out`. |
| `UAV_E2E_REPORT` | Override the JSON report path (default `{persistent}/unitedav-e2e-report.json`). |

Generate fixtures with `tests/media/gen.sh` (synthetic, public-domain) — it
produces both the VP9+Opus general clip and the H.264 clips the HW probe needs.

## Wiring it as the standalone Player's self-test scene

This is the scene the standalone Player builds and runs. Each OS runs its own
Player on its own host:

1. Import the sample (Package Manager → UnitedAV → Samples → "End To End").
2. Add `Assets/Samples/.../EndToEnd/EndToEnd.unity` to **Build Settings → Scenes
   In Build** as scene 0.
3. Ensure the correct native plugin is in `Plugins/<Platform>/` with its
   `PluginImporter` `.meta` before cross-building (`libUnitedAV.so` for Linux,
   `.dylib` for macOS, `UnitedAV.dll` for Windows).
4. Cross-build the Players for each target platform.
5. Run each Player **on its target host**, passing the env vars:
   - **Linux** (locally, under `xvfb` for a GPU/readback context):
     ```sh
     UAV_HWDECODE=auto UAV_TEST_MEDIA_DIR=/path/to/tests/media/out \
       xvfb-run -a ./EndToEnd.x86_64 -batchmode -nographics=false
       # then: echo "exit=$?"; cat ~/.config/unity3d/UnitedAV/UnitedAV/unitedav-e2e-report.json
     ```
     (Do **not** pass `-nographics` if you want the texture/HW cases to run — they
     need a graphics device. `xvfb` provides one headlessly.)
   - **Windows**: copy the Player to a Windows host and run `EndToEnd.exe` with the
     env vars; read `%USERPROFILE%\AppData\LocalLow\UnitedAV\UnitedAV\unitedav-e2e-report.json`.
   - **macOS**: run the `.app` on a macOS host.

A host cannot run another OS's Player — keep each RUN leg on its target.

## Report shape (JSON)

```jsonc
{
  "schema": 1,
  "platform": "LinuxPlayer",
  "graphicsDevice": "... / Vulkan",
  "nativePluginLoaded": true,
  "mediaPath": ".../webm__vp9__opus.webm",
  "hwMediaPath": ".../mp4__h264__aac.mp4",
  "hwDecodeEnv": "auto",
  "hwBackend": "vaapi",
  "overallPass": true,
  "passCount": 10, "failCount": 0, "skipCount": 0,
  "exitCode": 0,
  "cases": [ { "id": "open", "status": "Pass", "detail": "...", "blocker": "", "remediation": "", "durationMs": 412.3 }, ... ]
}
```

The report is also echoed to the Player log between `[uav-e2e] BEGIN REPORT` and
`[uav-e2e] END REPORT`, so a run leg with only stdout can still capture it.
