// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using UnityEngine;
using UnitedAV;
using Debug = UnityEngine.Debug;

namespace UnitedAV.Samples.EndToEnd
{
    [DisallowMultipleComponent]
    public sealed class IntegrationBattery : MonoBehaviour
    {
        [Header("Media")]
        [Tooltip("Optional explicit clip path/URL. If empty, a fixture is resolved " +
                 "from UAV_TEST_MEDIA_DIR or a repo-relative tests/media/out.")]
        public string mediaPathOverride = "";

        [Header("Run control")]
        [Tooltip("Start the battery automatically when the scene loads (standalone self-test).")]
        public bool autoRunOnStart = true;

        [Tooltip("Quit the application with an exit code when the battery finishes " +
                 "(set false in-editor to keep the editor alive).")]
        public bool quitOnFinish = true;

        [Tooltip("Per-case timeout. A case that cannot reach its condition in this " +
                 "long is SKIPPED with a blocker (never a fake pass).")]
        public float perCaseTimeoutSeconds = 20f;

        [Header("Outputs")]
        [Tooltip("Where the JSON report is written. {persistent} expands to " +
                 "Application.persistentDataPath. Override with UAV_E2E_REPORT.")]
        public string reportPath = "{persistent}/unitedav-e2e-report.json";

        private const string PreferredFixture = "webm__vp9__opus.webm";
        private static readonly string[] H264Fixtures =
            { "mp4__h264__aac.mp4", "mov__h264__aac.mov", "mpegts__h264__mp3.ts" };
        private static readonly string[] ClipExtensions =
            { "*.mp4", "*.webm", "*.mkv", "*.mov", "*.ts" };

        // The HW backend name is not exposed through the C# ABI; scan the Player log for it.
        private const string HwEnabledMarker = "[uav] hardware decode enabled:";

        private MediaPlayer _player;
        private BatteryReport _report;
        private bool _ran;

        private void Start()
        {
            if (autoRunOnStart)
                StartCoroutine(RunBattery());
        }

        public Coroutine Run() => StartCoroutine(RunBattery());

        private IEnumerator RunBattery()
        {
            if (_ran)
                yield break;
            _ran = true;

            _report = new BatteryReport
            {
                platform = Application.platform.ToString(),
                unityVersion = Application.unityVersion,
                graphicsDevice = SafeGraphicsName(),
                hwDecodeEnv = Environment.GetEnvironmentVariable("UAV_HWDECODE") ?? "unset",
            };

            Debug.Log("[uav-e2e] integration battery starting.");

            _player = gameObject.GetComponent<MediaPlayer>();
            if (_player == null)
                _player = gameObject.AddComponent<MediaPlayer>();
            _player.AutoOpen = false;

            _report.nativePluginLoaded = _player.Control != null && _player.Info != null;

            string clip = !string.IsNullOrEmpty(mediaPathOverride)
                ? mediaPathOverride
                : ResolveGeneralClip();
            _report.mediaPath = clip ?? "";

            bool decodeUsable = false;
            {
                var c = NewCase("open", "OpenMedia reaches READY with valid Info");
                var sw = Stopwatch.StartNew();

                if (clip == null)
                {
                    c.Set(CaseStatus.Skip,
                        "no media fixture resolved",
                        "No clip from UAV_TEST_MEDIA_DIR, mediaPathOverride, or tests/media/out.",
                        "Generate fixtures (tests/media/gen.sh) or set UAV_TEST_MEDIA_DIR to a folder with a clip.");
                }
                else
                {
                    bool opened = false;
                    try { opened = _player.OpenMedia(MediaPathType.AbsolutePathOrURL, clip, autoPlay: false); }
                    catch (Exception e)
                    {
                        c.Set(CaseStatus.Fail, "OpenMedia threw across the P/Invoke boundary",
                            e.GetType().Name + ": " + e.Message,
                            "OpenMedia must never throw; investigate the native uav_open path.");
                    }

                    if (c.statusEnum == CaseStatus.Pending)
                    {
                        if (!opened)
                        {
                            c.Set(CaseStatus.Skip, "native open failed",
                                "uav_open returned non-OK (plugin/FFmpeg absent or codec unsupported here).",
                                "Place the platform native plugin in Plugins/<Platform>/ and ensure the fixture codec decodes under LGPL FFmpeg.");
                        }
                        else
                        {
                            float deadline = Time.realtimeSinceStartup + perCaseTimeoutSeconds;
                            bool ready = false;
                            while (Time.realtimeSinceStartup < deadline)
                            {
                                if (_player.Info.HasVideo() &&
                                    _player.Info.GetVideoWidth() > 0 &&
                                    _player.Info.GetVideoHeight() > 0)
                                {
                                    ready = true;
                                    break;
                                }
                                yield return null;
                            }

                            if (ready)
                            {
                                decodeUsable = true;
                                c.Set(CaseStatus.Pass,
                                    $"READY: {_player.Info.GetVideoWidth()}x{_player.Info.GetVideoHeight()} " +
                                    $"@ {_player.Info.GetVideoFrameRate():0.##}fps, hasAudio={_player.Info.HasAudio()}");
                            }
                            else
                            {
                                c.Set(CaseStatus.Skip, "opened but never reached READY",
                                    "No valid dimensions within timeout (no decode backend active in this process).",
                                    "Ensure the native plugin + its FFmpeg are loadable by this Player; raise perCaseTimeoutSeconds for slow CI.");
                            }
                        }
                    }
                }

                sw.Stop();
                c.durationMs = sw.Elapsed.TotalMilliseconds;
            }

            yield return RunGated("play", "Play(): position advances and IsPlaying() is true",
                decodeUsable, PlayCase());

            yield return RunGated("pause", "Pause(): IsPaused() true and position freezes",
                decodeUsable, PauseCase());

            yield return RunGated("seek", "Seek(t): GetCurrentTime() moves toward t",
                decodeUsable, SeekCase());

            yield return RunGated("texture_nontrivial", "Decoded texture is non-trivial (not one flat color)",
                decodeUsable, TextureNonTrivialCase());

            yield return RunGated("events", "MediaPlayerEvent listener observes >=1 event; HasListeners()",
                decodeUsable, EventsCase());

            yield return RunGated("info_correctness", "Info dims>0, frame rate (0,120], finite duration>0, stable flags",
                decodeUsable, InfoCorrectnessCase());

            yield return RunGated("texture_contract", "Texture dims==Info, RequiresVerticalFlip stable, pixels non-trivial",
                decodeUsable, TextureContractCase());

            yield return RunGated("loop_wrap", "SetLooping(true): clock wraps past end (native loop, not just managed flag)",
                decodeUsable, LoopWrapCase());

            yield return RunGated("eof_finished", "Looping off: clip reaches IsFinished() and clock stops at end",
                decodeUsable, EofFinishedCase());

            yield return RunGated("bogus_open", "OpenMedia(nonexistent) returns false gracefully; player still usable after",
                decodeUsable, BogusOpenCase());

            yield return RunGated("reopen_stability", "open/close x5 on the fixture; no throw; final open reaches READY",
                decodeUsable, ReopenStabilityCase());

            yield return RunGated("codec_breadth", "Decode >=1 frame for >=2 distinct codecs across the fixture set",
                decodeUsable, CodecBreadthCase());

            RunManaged("loop", "SetLooping/IsLooping managed round-trip", LoopCase);
            RunManaged("volume", "AudioVolume clamps to [0,1] and round-trips", VolumeCase);
            RunManaged("mute", "Mute via AudioVolume=0 then restore (no public Mute API)", MuteCase);

            yield return HwBackendCase();

            if (_player.MediaOpened)
                _player.CloseMedia();

            FinalizeReport();
        }

        private IEnumerator RunGated(string id, string name, bool decodeUsable, IEnumerator body)
        {
            var c = NewCase(id, name);
            var sw = Stopwatch.StartNew();

            if (!decodeUsable)
            {
                c.Set(CaseStatus.Skip, "decode not usable",
                    "The 'open' case did not reach READY (no working native plugin / FFmpeg / codec here).",
                    "Resolve the 'open' blocker first; this case runs once a clip decodes.");
                sw.Stop();
                c.durationMs = sw.Elapsed.TotalMilliseconds;
                yield break;
            }

            _activeCase = c;
            yield return body;
            _activeCase = null;

            if (c.statusEnum == CaseStatus.Pending)
                c.Set(CaseStatus.Skip, "case did not classify",
                    "The case completed without reaching a definitive condition.",
                    "Inspect detail/log; consider raising perCaseTimeoutSeconds.");

            sw.Stop();
            c.durationMs = sw.Elapsed.TotalMilliseconds;
        }

        private void RunManaged(string id, string name, Action<CaseResult> body)
        {
            var c = NewCase(id, name);
            var sw = Stopwatch.StartNew();
            try { body(c); }
            catch (Exception e)
            {
                c.Set(CaseStatus.Fail, "case threw", e.GetType().Name + ": " + e.Message,
                    "Investigate the managed path; it must not throw.");
            }
            sw.Stop();
            c.durationMs = sw.Elapsed.TotalMilliseconds;
        }

        private CaseResult _activeCase;

        private IEnumerator PlayCase()
        {
            var c = _activeCase;
            var ctl = _player.Control;

            double t0 = ctl.GetCurrentTime();
            bool started = ctl.Play();
            if (!started)
            {
                c.Set(CaseStatus.Skip, "Play() returned false",
                    "Native uav_play rejected the call in this state.",
                    "Ensure the clip opened and the player is not in an error state.");
                yield break;
            }

            float deadline = Time.realtimeSinceStartup + perCaseTimeoutSeconds;
            bool advanced = false;
            bool sawPlaying = false;
            while (Time.realtimeSinceStartup < deadline)
            {
                if (ctl.IsPlaying()) sawPlaying = true;
                if (ctl.GetCurrentTime() > t0 + 1e-3)
                {
                    advanced = true;
                    break;
                }
                yield return null;
            }

            if (advanced)
                c.Set(CaseStatus.Pass, $"position advanced {t0:0.###}s -> {ctl.GetCurrentTime():0.###}s, IsPlaying={sawPlaying}");
            else
                c.Set(CaseStatus.Skip, "position did not advance",
                    "Clock never moved (paused at source, single-frame clip, or no audio/video clock).",
                    "Use a multi-second fixture; verify the native clock advances during playback.");
        }

        private IEnumerator PauseCase()
        {
            var c = _activeCase;
            var ctl = _player.Control;

            if (!ctl.IsPlaying())
                ctl.Play();

            float warm = Time.realtimeSinceStartup + 0.5f;
            while (Time.realtimeSinceStartup < warm)
                yield return null;

            bool paused = ctl.Pause();
            if (!paused)
            {
                c.Set(CaseStatus.Skip, "Pause() returned false",
                    "Native uav_pause rejected the call in this state.",
                    "Ensure playback was active before pausing.");
                yield break;
            }

            float settle = Time.realtimeSinceStartup + 0.3f;
            while (Time.realtimeSinceStartup < settle)
                yield return null;

            bool isPaused = ctl.IsPaused();
            double a = ctl.GetCurrentTime();
            float hold = Time.realtimeSinceStartup + 0.5f;
            while (Time.realtimeSinceStartup < hold)
                yield return null;
            double b = ctl.GetCurrentTime();

            bool frozen = Math.Abs(b - a) < 0.1;
            if (isPaused && frozen)
                c.Set(CaseStatus.Pass, $"IsPaused=true, clock held {a:0.###}s ~ {b:0.###}s");
            else if (isPaused && !frozen)
                c.Set(CaseStatus.Fail, $"IsPaused=true but clock advanced {a:0.###}s -> {b:0.###}s while paused",
                    "Paused playback must not advance the clock.",
                    "Investigate native pause: the worker thread should stop advancing the presentation clock.");
            else
                c.Set(CaseStatus.Skip, "state did not report Paused",
                    "uav_get_state never returned Paused after Pause() (timing/state model difference).",
                    "Confirm the native state machine surfaces a Paused state.");
        }

        private IEnumerator SeekCase()
        {
            var c = _activeCase;
            var ctl = _player.Control;
            var info = _player.Info;

            double dur = info.GetDuration();
            double target = double.IsInfinity(dur) || dur <= 0 ? 0.5 : Math.Min(dur * 0.5, dur - 0.25);
            if (target < 0.1) target = 0.1;

            bool ok = ctl.Seek(target);
            if (!ok)
            {
                c.Set(CaseStatus.Skip, "Seek() returned false",
                    "Native uav_seek rejected the call (non-seekable source or state).",
                    "Use a seekable local fixture; live streams may reject absolute seeks.");
                yield break;
            }

            ctl.Play();
            float deadline = Time.realtimeSinceStartup + perCaseTimeoutSeconds;
            double observed = ctl.GetCurrentTime();
            bool landed = false;
            while (Time.realtimeSinceStartup < deadline)
            {
                observed = ctl.GetCurrentTime();
                if (Math.Abs(observed - target) <= 1.0)
                {
                    landed = true;
                    break;
                }
                yield return null;
            }

            if (landed)
                c.Set(CaseStatus.Pass, $"sought to {target:0.###}s, observed {observed:0.###}s");
            else
                c.Set(CaseStatus.Skip, "clock did not reach the seek target",
                    $"Sought {target:0.###}s; observed {observed:0.###}s within timeout.",
                    "Use a seekable multi-second fixture; verify native seek repositions the clock.");
        }

        private IEnumerator TextureNonTrivialCase()
        {
            var c = _activeCase;
            var producer = _player.TextureProducer;

            _player.Control.Play();

            float deadline = Time.realtimeSinceStartup + perCaseTimeoutSeconds;
            Texture tex = null;
            while (Time.realtimeSinceStartup < deadline)
            {
                tex = producer.GetTexture();
                if (tex != null && tex.width > 0 && tex.height > 0)
                    break;
                yield return null;
            }

            if (tex == null || tex.width <= 0 || tex.height <= 0)
            {
                c.Set(CaseStatus.Skip, "no texture uploaded",
                    "GetTexture() never produced a sized texture (CPU upload path inactive / headless).",
                    "Run with a GPU-capable Player (xvfb on Linux) so LoadRawTextureData uploads frames.");
                yield break;
            }

            Color32[] pixels = null;
            string readbackError = null;
            try
            {
                var t2d = tex as Texture2D;
                if (t2d != null)
                {
                    pixels = t2d.GetPixels32();
                }
                else
                {
                    readbackError = "texture is not a Texture2D (GPU path)";
                }
            }
            catch (Exception e)
            {
                readbackError = e.GetType().Name + ": " + e.Message;
            }

            if (pixels == null || pixels.Length == 0)
            {
                if (SystemInfo.graphicsDeviceType != UnityEngine.Rendering.GraphicsDeviceType.Null)
                {
                    pixels = ReadbackViaBlit(tex, out readbackError);
                }
            }

            if (pixels == null || pixels.Length == 0)
            {
                c.Set(CaseStatus.Skip, "texture not readable",
                    "Could not read pixels (" + (readbackError ?? "no readback path") + ").",
                    "Run on a Player with a usable graphics device; CPU-path textures are readable via GetPixels32.");
                yield break;
            }

            if (IsNonTrivial(pixels, out int distinct, out int sampled))
                c.Set(CaseStatus.Pass, $"{tex.width}x{tex.height}, {distinct} distinct colors over {sampled} sampled pixels");
            else
                c.Set(CaseStatus.Fail, $"frame is a single flat color over {sampled} sampled pixels",
                    "A decoded frame should contain image detail, not one uniform color.",
                    "Verify the decode + RGBA upload path; a flat frame usually means a stride/format/upload mismatch.");
        }

        private IEnumerator EventsCase()
        {
            var c = _activeCase;
            var events = _player.Events;

            var seen = new List<MediaPlayerEvent.EventType>();
            // Delegate, not a local function, so this compiles inside an iterator.
            UnityEngine.Events.UnityAction<MediaPlayer, MediaPlayerEvent.EventType, ErrorCode> listener =
                (mp, et, code) => seen.Add(et);

            events.AddListener(listener);
            bool hasListeners = events.HasListeners();

            if (_player.MediaOpened)
                _player.CloseMedia();

            bool reopened = _player.OpenMedia(MediaPathType.AbsolutePathOrURL, _report.mediaPath, autoPlay: true);
            if (!reopened)
            {
                events.RemoveListener(listener);
                c.Set(CaseStatus.Skip, "re-open failed for event observation",
                    "Could not re-open the clip to drive a fresh event lifecycle.",
                    "Ensure the fixture re-opens; check native uav_open idempotency after close.");
                yield break;
            }

            float deadline = Time.realtimeSinceStartup + perCaseTimeoutSeconds;
            while (Time.realtimeSinceStartup < deadline && seen.Count == 0)
                yield return null;

            events.RemoveListener(listener);

            if (seen.Count > 0 && hasListeners)
                c.Set(CaseStatus.Pass, $"HasListeners=true; observed {seen.Count} event(s): {string.Join(",", seen)}");
            else if (!hasListeners)
                c.Set(CaseStatus.Fail, "HasListeners() returned false after AddListener",
                    "HasListeners() must report runtime-added listeners.",
                    "Check MediaPlayerEvent.AddListener runtime-count tracking.");
            else
                c.Set(CaseStatus.Skip, "no events fired",
                    "Listener attached but no MediaPlayerEvent arrived within timeout.",
                    "Needs a working decode lifecycle; resolve the 'open' blocker.");
        }

        private IEnumerator InfoCorrectnessCase()
        {
            var c = _activeCase;
            var info = _player.Info;

            if (!_player.MediaOpened)
            {
                bool reopened = _player.OpenMedia(MediaPathType.AbsolutePathOrURL, _report.mediaPath, autoPlay: false);
                if (!reopened)
                {
                    c.Set(CaseStatus.Skip, "could not re-open fixture for info read",
                        "OpenMedia failed when re-establishing the clip for the Info case.",
                        "Ensure the fixture re-opens after close; check native uav_open idempotency.");
                    yield break;
                }
            }

            float deadline = Time.realtimeSinceStartup + perCaseTimeoutSeconds;
            int w = 0, h = 0;
            while (Time.realtimeSinceStartup < deadline)
            {
                w = info.GetVideoWidth();
                h = info.GetVideoHeight();
                if (info.HasVideo() && w > 0 && h > 0)
                    break;
                yield return null;
            }

            if (!(info.HasVideo() && w > 0 && h > 0))
            {
                c.Set(CaseStatus.Skip, "metadata never became valid",
                    "Info dimensions stayed 0 within the timeout (no decode backend active).",
                    "Resolve the 'open' blocker; raise perCaseTimeoutSeconds on slow CI.");
                yield break;
            }

            float fps = info.GetVideoFrameRate();
            if (!(fps > 0f && fps <= 120f))
            {
                c.Set(CaseStatus.Fail, $"frame rate {fps:0.###} out of (0,120]",
                    "A local fixture must report a plausible frame rate.",
                    "Check native uav_get_info frame_rate (avg_frame_rate) reporting.");
                yield break;
            }

            double dur = info.GetDuration();
            if (double.IsInfinity(dur) || double.IsNaN(dur) || dur <= 0.0)
            {
                c.Set(CaseStatus.Fail, $"duration {dur} not finite/positive for a local file",
                    "A seekable local fixture must report a finite, positive duration; +inf is reserved for live.",
                    "Check native uav_get_info duration; only live/unknown should map to +inf.");
                yield break;
            }

            int w2 = info.GetVideoWidth();
            int h2 = info.GetVideoHeight();
            bool hasV2 = info.HasVideo();
            bool hasA1 = info.HasAudio();
            bool hasA2 = info.HasAudio();

            if (w2 != w || h2 != h || hasV2 != true || hasA2 != hasA1)
            {
                c.Set(CaseStatus.Fail,
                    $"Info unstable across reads: {w}x{h}/audio={hasA1} then {w2}x{h2}/audio={hasA2}",
                    "Repeated Info reads must return identical, stable values.",
                    "Check native uav_get_info for a race or uninitialized field.");
                yield break;
            }

            c.Set(CaseStatus.Pass,
                $"{w}x{h} @ {fps:0.##}fps, duration {dur:0.###}s, hasAudio={hasA1} (stable across reads)");
        }

        private IEnumerator TextureContractCase()
        {
            var c = _activeCase;
            var producer = _player.TextureProducer;
            var info = _player.Info;

            bool flip1 = producer.RequiresVerticalFlip();
            bool flip2 = producer.RequiresVerticalFlip();
            if (flip1 != flip2)
            {
                c.Set(CaseStatus.Fail, $"RequiresVerticalFlip unstable: {flip1} then {flip2}",
                    "RequiresVerticalFlip() must return a stable bool.",
                    "Check TextureProducerImpl.RequiresVerticalFlip — it should be deterministic.");
                yield break;
            }

            if (!_player.Control.IsPlaying())
                _player.Control.Play();

            float deadline = Time.realtimeSinceStartup + perCaseTimeoutSeconds;
            Texture tex = null;
            while (Time.realtimeSinceStartup < deadline)
            {
                tex = producer.GetTexture();
                if (tex != null && tex.width > 0 && tex.height > 0 &&
                    info.GetVideoWidth() > 0 && info.GetVideoHeight() > 0)
                    break;
                yield return null;
            }

            if (tex == null || tex.width <= 0 || tex.height <= 0)
            {
                if (SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.Null)
                {
                    c.Set(CaseStatus.Skip, "headless: no texture uploaded",
                        "graphicsDeviceType==Null; the CPU upload path produces no sized texture headlessly.",
                        "Run on a Player with a usable graphics device to exercise the texture contract.");
                }
                else
                {
                    c.Set(CaseStatus.Skip, "no texture uploaded",
                        "GetTexture() never produced a sized texture within the timeout.",
                        "Verify the decode + LoadRawTextureData upload path on this Player.");
                }
                yield break;
            }

            int iw = info.GetVideoWidth();
            int ih = info.GetVideoHeight();
            if (tex.width != iw || tex.height != ih)
            {
                c.Set(CaseStatus.Fail,
                    $"texture {tex.width}x{tex.height} != Info {iw}x{ih}",
                    "The decoded texture dimensions must match the reported Info dimensions.",
                    "Check the texture allocation in MediaPlayer.Video against uav_get_info width/height.");
                yield break;
            }

            if (SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.Null)
            {
                c.Set(CaseStatus.Skip, "headless: pixel readback unavailable",
                    "graphicsDeviceType==Null; cannot verify non-trivial pixels. Dims/flip contract held.",
                    "Run on a Player with a graphics device to verify decoded pixel content.");
                yield break;
            }

            Color32[] pixels = null;
            string readbackError = null;
            try
            {
                var t2d = tex as Texture2D;
                if (t2d != null)
                    pixels = t2d.GetPixels32();
            }
            catch (Exception e)
            {
                readbackError = e.GetType().Name + ": " + e.Message;
            }

            if (pixels == null || pixels.Length == 0)
                pixels = ReadbackViaBlit(tex, out readbackError);

            if (pixels == null || pixels.Length == 0)
            {
                c.Set(CaseStatus.Skip, "texture not readable on this device",
                    "Could not read pixels (" + (readbackError ?? "no readback path") + "); dims/flip contract held.",
                    "Run on a device where GetPixels32 or a Blit readback succeeds.");
                yield break;
            }

            if (IsNonTrivial(pixels, out int distinct, out int sampled))
                c.Set(CaseStatus.Pass,
                    $"texture {tex.width}x{tex.height}==Info, flip={flip1} stable, {distinct} distinct colors / {sampled} sampled");
            else
                c.Set(CaseStatus.Fail,
                    $"texture {tex.width}x{tex.height}==Info but a single flat color over {sampled} sampled pixels",
                    "A decoded frame should contain image detail, not one uniform color.",
                    "Verify the decode + RGBA upload path; a flat frame implies a stride/format/upload mismatch.");
        }

        private IEnumerator LoopWrapCase()
        {
            var c = _activeCase;
            var ctl = _player.Control;
            var info = _player.Info;

            double dur = info.GetDuration();
            if (double.IsInfinity(dur) || dur <= 0.0)
            {
                c.Set(CaseStatus.Skip, "unknown/zero duration",
                    "Cannot test wrap-around without a finite clip duration.",
                    "Use a finite, seekable local fixture.");
                yield break;
            }

            ctl.SetLooping(true);
            ctl.Seek(0.0);
            ctl.Play();

            float budget = (float)Math.Min(perCaseTimeoutSeconds, dur * 2.5 + 3.0);
            float deadline = Time.realtimeSinceStartup + budget;

            double nearEnd = dur * 0.75;
            bool climbedNearEnd = false;
            double peak = 0.0;
            bool wrapped = false;
            double prev = ctl.GetCurrentTime();

            while (Time.realtimeSinceStartup < deadline)
            {
                double now = ctl.GetCurrentTime();
                if (now > peak) peak = now;
                if (now >= nearEnd) climbedNearEnd = true;

                if (climbedNearEnd && now < prev - (dur * 0.5))
                {
                    wrapped = true;
                    break;
                }
                prev = now;
                yield return null;
            }

            if (wrapped)
            {
                ctl.SetLooping(false);
                c.Set(CaseStatus.Pass,
                    $"clock climbed to ~{peak:0.###}s (dur {dur:0.###}s) then wrapped back to {ctl.GetCurrentTime():0.###}s");
            }
            else if (!climbedNearEnd)
            {
                ctl.SetLooping(false);
                c.Set(CaseStatus.Skip, "clip never reached near its end in time",
                    $"peak {peak:0.###}s of {dur:0.###}s within {budget:0.#}s budget; cannot judge looping.",
                    "Use a short fixture or raise perCaseTimeoutSeconds so a full clip length plays.");
            }
            else
            {
                ctl.SetLooping(false);
                c.Set(CaseStatus.Fail,
                    $"reached end (~{peak:0.###}s of {dur:0.###}s) with looping ON but the clock did not wrap",
                    "SetLooping(true) must make native playback wrap to the start, not latch at the end.",
                    "Investigate native uav_set_looping: the worker should restart the stream at EOF.");
            }
        }

        private IEnumerator EofFinishedCase()
        {
            var c = _activeCase;
            var ctl = _player.Control;
            var info = _player.Info;

            double dur = info.GetDuration();
            if (double.IsInfinity(dur) || dur <= 0.0)
            {
                c.Set(CaseStatus.Skip, "unknown/zero duration",
                    "Cannot test end-of-stream without a finite clip duration.",
                    "Use a finite local fixture.");
                yield break;
            }

            ctl.SetLooping(false);
            ctl.Seek(0.0);
            ctl.Play();

            float budget = (float)Math.Min(perCaseTimeoutSeconds, dur * 2.5 + 3.0);
            float deadline = Time.realtimeSinceStartup + budget;

            double nearEnd = dur * 0.75;
            bool climbedNearEnd = false;
            double peak = 0.0;
            bool finished = false;

            while (Time.realtimeSinceStartup < deadline)
            {
                double now = ctl.GetCurrentTime();
                if (now > peak) peak = now;
                if (now >= nearEnd) climbedNearEnd = true;
                if (ctl.IsFinished())
                {
                    finished = true;
                    break;
                }
                yield return null;
            }

            if (!climbedNearEnd && !finished)
            {
                c.Set(CaseStatus.Skip, "clip never reached near its end in time",
                    $"peak {peak:0.###}s of {dur:0.###}s within {budget:0.#}s budget; cannot judge EOF.",
                    "Use a short fixture or raise perCaseTimeoutSeconds so the clip plays to the end.");
                yield break;
            }

            if (!finished)
            {
                c.Set(CaseStatus.Fail,
                    $"clock reached end (~{peak:0.###}s of {dur:0.###}s) with looping OFF but IsFinished() never tripped",
                    "Non-looping playback must report IsFinished() at end-of-stream.",
                    "Investigate the native Finished state / EOF handling.");
                yield break;
            }

            double a = ctl.GetCurrentTime();
            float hold = Time.realtimeSinceStartup + 0.5f;
            while (Time.realtimeSinceStartup < hold)
                yield return null;
            double b = ctl.GetCurrentTime();

            if (b < a - (dur * 0.5))
            {
                c.Set(CaseStatus.Fail,
                    $"after IsFinished() the clock wrapped {a:0.###}s -> {b:0.###}s with looping OFF",
                    "With looping OFF the player must not restart after finishing.",
                    "Investigate native EOF handling: it should hold at end, not loop.");
                yield break;
            }

            if (Math.Abs(b - a) < 0.25)
                c.Set(CaseStatus.Pass,
                    $"IsFinished()=true at ~{peak:0.###}s (dur {dur:0.###}s); clock held {a:0.###}s ~ {b:0.###}s");
            else
                c.Set(CaseStatus.Fail,
                    $"IsFinished()=true but clock kept advancing {a:0.###}s -> {b:0.###}s",
                    "A finished, non-looping clip must not keep advancing its clock.",
                    "Investigate native Finished state: the presentation clock should stop at EOF.");
        }

        private IEnumerator BogusOpenCase()
        {
            var c = _activeCase;

            if (_player.MediaOpened)
                _player.CloseMedia();

            string bogus = Path.Combine(
                Path.GetDirectoryName(_report.mediaPath) ?? ".",
                "uav_nonexistent_" + Guid.NewGuid().ToString("N") + ".mp4");

            bool bogusResult = true;
            bool threw = false;
            try
            {
                bogusResult = _player.OpenMedia(MediaPathType.AbsolutePathOrURL, bogus, autoPlay: false);
            }
            catch (Exception e)
            {
                threw = true;
                c.Set(CaseStatus.Fail, "OpenMedia threw on a nonexistent path",
                    e.GetType().Name + ": " + e.Message,
                    "OpenMedia must return false for a bad path, never throw/crash.");
            }

            if (threw)
                yield break;

            if (bogusResult)
            {
                c.Set(CaseStatus.Fail, "OpenMedia(nonexistent) returned true",
                    "Opening a path that does not exist must fail (return false).",
                    "Check native uav_open: it must reject unopenable inputs with a non-OK result.");
                yield break;
            }

            bool realOpen = false;
            try
            {
                realOpen = _player.OpenMedia(MediaPathType.AbsolutePathOrURL, _report.mediaPath, autoPlay: false);
            }
            catch (Exception e)
            {
                c.Set(CaseStatus.Fail, "real OpenMedia threw after a bogus open",
                    e.GetType().Name + ": " + e.Message,
                    "A failed open must leave the player usable; the next real open must not throw.");
                yield break;
            }

            if (!realOpen)
            {
                c.Set(CaseStatus.Fail, "player unusable after a bogus open",
                    "The real fixture failed to open after a nonexistent-path open returned false.",
                    "A failed open must not corrupt the native handle; check uav_open error recovery.");
                yield break;
            }

            float deadline = Time.realtimeSinceStartup + perCaseTimeoutSeconds;
            bool ready = false;
            while (Time.realtimeSinceStartup < deadline)
            {
                if (_player.Info.HasVideo() &&
                    _player.Info.GetVideoWidth() > 0 && _player.Info.GetVideoHeight() > 0)
                {
                    ready = true;
                    break;
                }
                yield return null;
            }

            if (ready)
                c.Set(CaseStatus.Pass,
                    "bogus open returned false (no throw); subsequent real open reached READY " +
                    $"{_player.Info.GetVideoWidth()}x{_player.Info.GetVideoHeight()}");
            else
                c.Set(CaseStatus.Fail, "player did not recover to READY after a bogus open",
                    "Real open returned true but never reached READY after the failed open.",
                    "Check native handle state after a failed uav_open; it must remain fully usable.");
        }

        private IEnumerator ReopenStabilityCase()
        {
            var c = _activeCase;
            const int cycles = 5;

            for (int i = 0; i < cycles; i++)
            {
                if (_player.MediaOpened)
                {
                    try { _player.CloseMedia(); }
                    catch (Exception e)
                    {
                        c.Set(CaseStatus.Fail, $"CloseMedia threw on cycle {i + 1}",
                            e.GetType().Name + ": " + e.Message,
                            "Close must never throw; investigate native uav_close.");
                        yield break;
                    }
                }

                bool opened = false;
                try
                {
                    opened = _player.OpenMedia(MediaPathType.AbsolutePathOrURL, _report.mediaPath, autoPlay: false);
                }
                catch (Exception e)
                {
                    c.Set(CaseStatus.Fail, $"OpenMedia threw on cycle {i + 1}",
                        e.GetType().Name + ": " + e.Message,
                        "Open must never throw; investigate native uav_open across reopen cycles.");
                    yield break;
                }

                if (!opened)
                {
                    c.Set(CaseStatus.Fail, $"OpenMedia returned false on cycle {i + 1} of {cycles}",
                        "The real fixture must re-open on every cycle.",
                        "Check native uav_open/uav_close idempotency across repeated cycles.");
                    yield break;
                }

                yield return null;
            }

            float deadline = Time.realtimeSinceStartup + perCaseTimeoutSeconds;
            bool ready = false;
            while (Time.realtimeSinceStartup < deadline)
            {
                if (_player.Info.HasVideo() &&
                    _player.Info.GetVideoWidth() > 0 && _player.Info.GetVideoHeight() > 0)
                {
                    ready = true;
                    break;
                }
                yield return null;
            }

            if (ready)
                c.Set(CaseStatus.Pass,
                    $"{cycles} open/close cycles with no throw; final open READY " +
                    $"{_player.Info.GetVideoWidth()}x{_player.Info.GetVideoHeight()}");
            else
                c.Set(CaseStatus.Fail,
                    $"survived {cycles} cycles but the final open never reached READY",
                    "After repeated reopen cycles the final open must still decode metadata.",
                    "Check for native state leakage across uav_open/uav_close cycles.");
        }

        private IEnumerator CodecBreadthCase()
        {
            var c = _activeCase;

            var fixtures = ResolveCodecFixtures();
            if (fixtures.Count == 0)
            {
                c.Set(CaseStatus.Skip, "no codec fixtures found",
                    "No recognizable codec fixtures in the media dir(s).",
                    "Generate fixtures (tests/media/gen.sh) or set UAV_TEST_MEDIA_DIR.");
                yield break;
            }

            float perCodec = (float)Math.Min(perCaseTimeoutSeconds, 8.0);

            var decoded = new List<string>();
            var notes = new List<string>();

            var codecs = new List<string>(fixtures.Keys);
            codecs.Sort(StringComparer.Ordinal);

            foreach (var codec in codecs)
            {
                string path = fixtures[codec];

                if (_player.MediaOpened)
                {
                    try { _player.CloseMedia(); } catch { }
                }

                bool opened = false;
                try { opened = _player.OpenMedia(MediaPathType.AbsolutePathOrURL, path, autoPlay: true); }
                catch (Exception e)
                {
                    notes.Add($"{codec}:open-threw({e.GetType().Name})");
                    continue;
                }

                if (!opened)
                {
                    notes.Add($"{codec}:open-failed");
                    continue;
                }

                float deadline = Time.realtimeSinceStartup + perCodec;
                bool frame = false;
                while (Time.realtimeSinceStartup < deadline)
                {
                    if (_player.Info.HasVideo() &&
                        _player.Info.GetVideoWidth() > 0 &&
                        _player.Info.GetVideoHeight() > 0 &&
                        _player.TextureProducer.GetTexture() != null)
                    {
                        frame = true;
                        break;
                    }
                    yield return null;
                }

                if (frame)
                {
                    decoded.Add(codec);
                    notes.Add($"{codec}:{_player.Info.GetVideoWidth()}x{_player.Info.GetVideoHeight()}");
                }
                else
                {
                    notes.Add($"{codec}:no-frame");
                }
            }

            if (_player.MediaOpened)
            {
                try { _player.CloseMedia(); } catch { }
            }

            string summary = string.Join(", ", notes);
            if (decoded.Count >= 2)
                c.Set(CaseStatus.Pass,
                    $"decoded {decoded.Count} codecs [{string.Join(",", decoded)}]; details: {summary}");
            else
                c.Set(CaseStatus.Skip, "fewer than 2 codecs decoded",
                    $"Only {decoded.Count} codec(s) decoded ([{string.Join(",", decoded)}]); breadth needs >=2. Details: {summary}",
                    "Provide >=2 decodable codec fixtures and ensure LGPL FFmpeg has those decoders.");
        }

        private Dictionary<string, string> ResolveCodecFixtures()
        {
            var map = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var dir in CandidateMediaDirs())
            {
                foreach (var ext in ClipExtensions)
                {
                    var hits = SafeGetFiles(dir, ext);
                    if (hits == null) continue;
                    Array.Sort(hits, StringComparer.Ordinal);
                    foreach (var path in hits)
                    {
                        string codec = CodecFromFixtureName(Path.GetFileName(path));
                        if (string.IsNullOrEmpty(codec) || codec == "novideo")
                            continue;
                        if (!map.ContainsKey(codec))
                            map[codec] = path;
                    }
                }
                if (map.Count > 0)
                    break;
            }
            return map;
        }

        private static string CodecFromFixtureName(string fileName)
        {
            if (string.IsNullOrEmpty(fileName))
                return null;
            string stem = Path.GetFileNameWithoutExtension(fileName);
            string[] parts = stem.Split(new[] { "__" }, StringSplitOptions.None);
            if (parts.Length < 2)
                return null;
            string codecField = parts[1];
            int us = codecField.IndexOf('_');
            return us > 0 ? codecField.Substring(0, us) : codecField;
        }

        private void LoopCase(CaseResult c)
        {
            var ctl = _player.Control;
            bool def = ctl.IsLooping();
            ctl.SetLooping(true);
            bool on = ctl.IsLooping();
            ctl.SetLooping(false);
            bool off = ctl.IsLooping();

            if (!def && on && !off)
                c.Set(CaseStatus.Pass, "default=false -> set true -> set false round-trips");
            else
                c.Set(CaseStatus.Fail, $"looping round-trip wrong: default={def}, on={on}, off={off}",
                    "IsLooping() must mirror the last SetLooping() value (ABI is set-only).",
                    "Check MediaControlImpl/MediaPlayer.LoopingFlag caching.");
        }

        private void VolumeCase(CaseResult c)
        {
            _player.AudioVolume = 2.5f;
            float hi = _player.AudioVolume;
            _player.AudioVolume = -1f;
            float lo = _player.AudioVolume;
            _player.AudioVolume = 0.5f;
            float mid = _player.AudioVolume;

            bool ok = Mathf.Approximately(hi, 1f) && Mathf.Approximately(lo, 0f) && Mathf.Approximately(mid, 0.5f);
            if (ok)
                c.Set(CaseStatus.Pass, "2.5->1, -1->0, 0.5->0.5 (clamped to [0,1])");
            else
                c.Set(CaseStatus.Fail, $"clamp/round-trip wrong: hi={hi}, lo={lo}, mid={mid}",
                    "AudioVolume must clamp to [0,1] and round-trip in range.",
                    "Check MediaPlayer.AudioVolume Mathf.Clamp01.");
        }

        private void MuteCase(CaseResult c)
        {
            float prior = _player.AudioVolume;
            _player.AudioVolume = 0f;
            float muted = _player.AudioVolume;
            _player.AudioVolume = prior;
            float restored = _player.AudioVolume;

            bool ok = Mathf.Approximately(muted, 0f) && Mathf.Approximately(restored, prior);
            if (ok)
                c.Set(CaseStatus.Pass, $"AudioVolume 0 (muted) -> {restored} (restored); no public Mute API, AudioVolume is the mute path");
            else
                c.Set(CaseStatus.Fail, $"mute round-trip wrong: muted={muted}, restored={restored} (prior={prior})",
                    "Silencing via AudioVolume=0 and restoring must round-trip.",
                    "Check MediaPlayer.AudioVolume setter (it pushes uav_set_volume and the AudioSource volume).");
        }

        private IEnumerator HwBackendCase()
        {
            var c = NewCase("hw_backend", "Hardware decode engages on an H.264 fixture (UAV_HWDECODE=auto)");
            var sw = Stopwatch.StartNew();

            string hwEnv = _report.hwDecodeEnv;
            string h264 = ResolveH264Clip();
            _report.hwMediaPath = h264 ?? "";

            string logPath = SafeConsoleLogPath();

            if (h264 == null)
            {
                c.Set(CaseStatus.Skip, "no H.264 fixture",
                    "HW decode only engages on H.264 here; VP8/VP9/AV1 silently fall back to SW.",
                    "Generate H.264 fixtures (tests/media/gen.sh produces mp4__h264__aac.mp4) or set UAV_TEST_MEDIA_DIR.");
                FinishHwCase(c, sw);
                yield break;
            }

            if (!string.Equals(hwEnv, "auto", StringComparison.OrdinalIgnoreCase) &&
                !string.Equals(hwEnv, "1", StringComparison.Ordinal))
            {
                c.Set(CaseStatus.Skip, "UAV_HWDECODE not set to auto",
                    $"UAV_HWDECODE={hwEnv}; hardware decode is not requested so SW is expected.",
                    "Launch the Player with UAV_HWDECODE=auto to request hardware decode.");
                FinishHwCase(c, sw);
                yield break;
            }

            int markerBefore = CountMarkerLines(logPath);

            if (_player.MediaOpened)
                _player.CloseMedia();

            bool opened = _player.OpenMedia(MediaPathType.AbsolutePathOrURL, h264, autoPlay: true);
            if (!opened)
            {
                c.Set(CaseStatus.Skip, "H.264 open failed",
                    "uav_open failed for the H.264 fixture (no H.264 decoder / plugin in this Player).",
                    "Ensure the native plugin + LGPL FFmpeg with an H.264 decoder is loadable here.");
                FinishHwCase(c, sw);
                yield break;
            }

            float deadline = Time.realtimeSinceStartup + perCaseTimeoutSeconds;
            bool decoded = false;
            while (Time.realtimeSinceStartup < deadline)
            {
                if (_player.Info.HasVideo() && _player.Info.GetVideoWidth() > 0 &&
                    _player.TextureProducer.GetTexture() != null)
                {
                    decoded = true;
                    break;
                }
                yield return null;
            }

            float flush = Time.realtimeSinceStartup + 0.5f;
            while (Time.realtimeSinceStartup < flush)
                yield return null;

            string backend = ScanForHwBackend(logPath, markerBefore);
            _report.hwBackend = backend ?? "";

            if (!string.IsNullOrEmpty(backend))
            {
                c.Set(CaseStatus.Pass,
                    $"native reported '{HwEnabledMarker} {backend}' decoding {Path.GetFileName(h264)}");
            }
            else if (!decoded)
            {
                c.Set(CaseStatus.Skip, "H.264 never decoded",
                    "Opened the H.264 fixture but no frame decoded (no H.264 decoder active).",
                    "Verify the native plugin's FFmpeg has an H.264 decoder and a GPU is present for HW.");
            }
            else
            {
                c.Set(CaseStatus.Skip, "decoded in software (no HW backend engaged)",
                    "No '" + HwEnabledMarker + "' line found in the Player log after decoding H.264; " +
                    "this host has no usable VAAPI/VideoToolbox/CUDA path.",
                    "Run on a host with a hardware H.264 decoder (Linux VAAPI / macOS VideoToolbox / NVIDIA NVDEC) " +
                    "and check the Player log at: " + (logPath ?? "<unknown>"));
            }

            FinishHwCase(c, sw);
        }

        private void FinishHwCase(CaseResult c, Stopwatch sw)
        {
            sw.Stop();
            c.durationMs = sw.Elapsed.TotalMilliseconds;
        }

        private string ScanForHwBackend(string logPath, int skipFirstN)
        {
            if (string.IsNullOrEmpty(logPath) || !File.Exists(logPath))
                return null;

            try
            {
                int seen = 0;
                string lastBackend = null;
                // Shared access: the Player is actively writing this file.
                using (var fs = new FileStream(logPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                using (var reader = new StreamReader(fs))
                {
                    string line;
                    while ((line = reader.ReadLine()) != null)
                    {
                        int idx = line.IndexOf(HwEnabledMarker, StringComparison.Ordinal);
                        if (idx < 0)
                            continue;
                        seen++;
                        if (seen <= skipFirstN)
                            continue;
                        string token = ExtractBackendToken(line, idx);
                        if (!string.IsNullOrEmpty(token))
                            lastBackend = token;
                    }
                }
                return lastBackend;
            }
            catch
            {
                return null;
            }
        }

        private int CountMarkerLines(string logPath)
        {
            if (string.IsNullOrEmpty(logPath) || !File.Exists(logPath))
                return 0;
            try
            {
                int n = 0;
                using (var fs = new FileStream(logPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                using (var reader = new StreamReader(fs))
                {
                    string line;
                    while ((line = reader.ReadLine()) != null)
                        if (line.IndexOf(HwEnabledMarker, StringComparison.Ordinal) >= 0)
                            n++;
                }
                return n;
            }
            catch { return 0; }
        }

        private static string ExtractBackendToken(string line, int markerIdx)
        {
            int start = markerIdx + HwEnabledMarker.Length;
            if (start >= line.Length)
                return null;
            string tail = line.Substring(start).Trim();
            if (tail.Length == 0)
                return null;
            int sp = tail.IndexOfAny(new[] { ' ', '\t', ')', ']', ',' });
            return sp > 0 ? tail.Substring(0, sp) : tail;
        }

        private static bool IsNonTrivial(Color32[] pixels, out int distinct, out int sampled)
        {
            distinct = 0;
            sampled = 0;
            if (pixels == null || pixels.Length == 0)
                return false;

            int target = Math.Min(4096, pixels.Length);
            int step = Math.Max(1, pixels.Length / target);

            var seen = new HashSet<int>();
            for (int i = 0; i < pixels.Length; i += step)
            {
                var p = pixels[i];
                int key = (p.r << 16) | (p.g << 8) | p.b;
                seen.Add(key);
                sampled++;
            }
            distinct = seen.Count;
            return distinct > 1;
        }

        private static Color32[] ReadbackViaBlit(Texture tex, out string error)
        {
            error = null;
            RenderTexture rt = null;
            RenderTexture prev = RenderTexture.active;
            Texture2D tmp = null;
            try
            {
                int w = Mathf.Min(tex.width, 256);
                int h = Mathf.Min(tex.height, 256);
                rt = RenderTexture.GetTemporary(w, h, 0, RenderTextureFormat.ARGB32);
                Graphics.Blit(tex, rt);
                RenderTexture.active = rt;
                tmp = new Texture2D(w, h, TextureFormat.RGBA32, false, false);
                tmp.ReadPixels(new Rect(0, 0, w, h), 0, 0);
                tmp.Apply(false);
                return tmp.GetPixels32();
            }
            catch (Exception e)
            {
                error = e.GetType().Name + ": " + e.Message;
                return null;
            }
            finally
            {
                RenderTexture.active = prev;
                if (rt != null) RenderTexture.ReleaseTemporary(rt);
                if (tmp != null) Destroy(tmp);
            }
        }

        private void FinalizeReport()
        {
            foreach (var c in _report.cases)
            {
                switch (c.statusEnum)
                {
                    case CaseStatus.Pass: _report.passCount++; break;
                    case CaseStatus.Fail: _report.failCount++; break;
                    case CaseStatus.Skip: _report.skipCount++; break;
                }
            }

            _report.overallPass = _report.failCount == 0 && _report.skipCount == 0 && _report.passCount > 0;

            int exit;
            if (_report.failCount > 0) exit = 1;
            else if (_report.skipCount > 0) exit = 2;
            else exit = 0;
            _report.exitCode = exit;

            string json = JsonUtility.ToJson(_report, prettyPrint: true);
            string outPath = ResolveReportPath();
            bool wrote = false;
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(outPath));
                File.WriteAllText(outPath, json);
                wrote = true;
            }
            catch (Exception e)
            {
                Debug.LogError($"[uav-e2e] failed to write report to {outPath}: {e.Message}");
            }

            Debug.Log("[uav-e2e] BEGIN REPORT\n" + json + "\n[uav-e2e] END REPORT");
            Debug.Log($"[uav-e2e] summary: pass={_report.passCount} fail={_report.failCount} " +
                      $"skip={_report.skipCount} exit={exit} report={(wrote ? outPath : "<not written>")}");

            if (quitOnFinish)
            {
                Debug.Log($"[uav-e2e] quitting with exit code {exit}.");
                Application.Quit(exit);
#if UNITY_EDITOR
                UnityEditor.EditorApplication.isPlaying = false;
#endif
            }
        }

        private CaseResult NewCase(string id, string name)
        {
            var c = new CaseResult { id = id, name = name, status = CaseStatus.Pending.ToString() };
            _report.cases.Add(c);
            return c;
        }

        private string ResolveReportPath()
        {
            string env = Environment.GetEnvironmentVariable("UAV_E2E_REPORT");
            string p = !string.IsNullOrEmpty(env) ? env : reportPath;
            p = p.Replace("{persistent}", Application.persistentDataPath);
            if (!Path.IsPathRooted(p))
                p = Path.Combine(Application.persistentDataPath, p);
            return p;
        }

        private static string SafeGraphicsName()
        {
            try
            {
                if (SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.Null)
                    return "null (headless)";
                return SystemInfo.graphicsDeviceName + " / " + SystemInfo.graphicsDeviceType;
            }
            catch { return "unknown"; }
        }

        private static string SafeConsoleLogPath()
        {
            try { return Application.consoleLogPath; }
            catch { return null; }
        }

        private string ResolveGeneralClip()
        {
            foreach (var dir in CandidateMediaDirs())
            {
                string preferred = Path.Combine(dir, PreferredFixture);
                if (File.Exists(preferred))
                    return preferred;
                foreach (var ext in ClipExtensions)
                {
                    var hits = SafeGetFiles(dir, ext);
                    if (hits != null && hits.Length > 0)
                    {
                        Array.Sort(hits, StringComparer.Ordinal);
                        return hits[0];
                    }
                }
            }
            return null;
        }

        private string ResolveH264Clip()
        {
            foreach (var dir in CandidateMediaDirs())
            {
                foreach (var name in H264Fixtures)
                {
                    string p = Path.Combine(dir, name);
                    if (File.Exists(p))
                        return p;
                }
            }
            return null;
        }

        private IEnumerable<string> CandidateMediaDirs()
        {
            string env = Environment.GetEnvironmentVariable("UAV_TEST_MEDIA_DIR");
            if (!string.IsNullOrEmpty(env) && Directory.Exists(env))
                yield return env;

            string probe = SafeDataPath();
            for (int i = 0; i < 6 && !string.IsNullOrEmpty(probe); i++)
            {
                string candidate = Path.Combine(probe, "tests", "media", "out");
                if (Directory.Exists(candidate))
                    yield return candidate;
                try { probe = Path.GetDirectoryName(probe); }
                catch { break; }
            }
        }

        private static string SafeDataPath()
        {
            try { return Application.dataPath; }
            catch { return null; }
        }

        private static string[] SafeGetFiles(string dir, string pattern)
        {
            try
            {
                if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
                    return null;
                return Directory.GetFiles(dir, pattern, SearchOption.TopDirectoryOnly);
            }
            catch { return null; }
        }
    }
}
