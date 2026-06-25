// SPDX-License-Identifier: Apache-2.0
// UnitedAV — PlayMode LIVE OBS broadcast e2e test.
//
// This is the Unity end of the REAL broadcast pipeline driven by
// unitedav/tests/streaming/obs-e2e.sh:
//
//   OBS Studio (headless) --RTMP--> mediamtx --HLS--> (this) MediaPlayer
//
// The orchestration script starts mediamtx + OBS BEFORE launching the editor, so
// by the time this test runs there is a live HLS endpoint at
//   http://127.0.0.1:8889/live/test/index.m3u8
// carrying OBS's 320x240 H.264/AAC broadcast.
//
// The test opens that URL with the UnitedAV MediaPlayer and asserts:
//   * READY: reaches metadata (HasVideo + width>0), at the broadcast 320x240.
//   * LIVE: GetDuration() is +inf (the live-stream signal: native dur<=0).
//   * PLAYING: with an advancing playback position (decode progressing on the
//     live wire, not a fixed file).
// Texture asserts are GUARDED for -nographics (decode is verified via metadata +
// advancing position regardless of GPU upload).
//
// If the live endpoint is absent (script not running / OBS not publishing) the
// test is INCONCLUSIVE -> Assert.Ignore, so a bare `-runTests` doesn't hard-fail.
// The script gates the run on OBS publishing, so under orchestration it asserts
// for real.

using System.Collections;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using UnityEngine.Networking;
using UnitedAV;

namespace UnitedAV.Tests.PlayMode
{
    public class ObsLiveStreamTests
    {
        // The live HLS URL OBS publishes via mediamtx (see obs-e2e.sh / mediamtx.yml).
        private const string LiveHlsUrl = "http://127.0.0.1:8889/live/test/index.m3u8";
        private const int ExpectedWidth = 320;
        private const int ExpectedHeight = 240;

        private GameObject _go;
        private MediaPlayer _player;

        [SetUp]
        public void SetUp()
        {
            _go = new GameObject("ObsLiveMediaPlayerHost");
            _player = _go.AddComponent<MediaPlayer>();
        }

        [TearDown]
        public void TearDown()
        {
            if (_player != null)
                _player.CloseMedia();
            if (_go != null)
                Object.Destroy(_go);
        }

        [UnityTest]
        public IEnumerator Opens_Live_Obs_Hls_Reaches_Ready_Then_Playing_As_Live()
        {
            // ---- endpoint reachability (used only to Ignore — not to fail) ----
            // mediamtx starts its HLS muxer on first read, and a cold editor's
            // first-frame networking can be slow, so retry over ~20s. A non-2xx
            // (ProtocolError) still proves the server is THERE, so we treat any
            // completed response (not a connection error) as reachable.
            bool reachable = false;
            for (int attempt = 0; attempt < 10 && !reachable; attempt++)
            {
                using (UnityWebRequest req = UnityWebRequest.Get(LiveHlsUrl))
                {
                    req.timeout = 3;
                    UnityWebRequestAsyncOperation op = req.SendWebRequest();
                    while (!op.isDone)
                        yield return null;
                    // Success OR ProtocolError (got an HTTP status) == server reachable.
                    reachable = req.result == UnityWebRequest.Result.Success
                                || req.result == UnityWebRequest.Result.ProtocolError;
                }
                if (!reachable)
                    for (int i = 0; i < 60; i++) yield return null; // ~1s backoff
            }

            // ---- open the live stream ----
            // Our native FFmpeg HLS reader tolerates muxer startup, so we open
            // regardless of the probe; we only Ignore (below) if metadata never
            // arrives AND the probe never saw the server (broadcast not running).
            bool opened = _player.OpenMedia(MediaPathType.AbsolutePathOrURL, LiveHlsUrl, autoPlay: true);
            Assert.IsTrue(opened, "OpenMedia returned false (native open of live HLS failed)");

            // ---- READY: metadata valid (HasVideo + dims) ----
            bool gotMetadata = false;
            for (int i = 0; i < 1800; i++) // generous for live HLS segment fetch (~30s)
            {
                if (_player.Info.HasVideo() && _player.Info.GetVideoWidth() > 0)
                {
                    gotMetadata = true;
                    break;
                }
                yield return null;
            }
            if (!gotMetadata)
            {
                _player.CloseMedia();
                if (!reachable)
                {
                    Assert.Ignore(
                        "Live OBS HLS endpoint not reachable at " + LiveHlsUrl +
                        " — start the broadcast first (tests/streaming/obs-unity-e2e.sh).");
                    yield break;
                }
                Assert.Fail("Server reachable but never reached READY (no metadata) for live HLS");
            }

            IMediaInfo info = _player.Info;
            Assert.AreEqual(ExpectedWidth, info.GetVideoWidth(), "live video width");
            Assert.AreEqual(ExpectedHeight, info.GetVideoHeight(), "live video height");
            Assert.IsTrue(info.HasVideo(), "HasVideo");
            // OBS broadcasts an AAC track; the relay carries it.
            Assert.IsTrue(info.HasAudio(), "HasAudio (OBS AAC track expected)");

            // ---- LIVE behavior: duration is +inf (native dur<=0 => live) ----
            double dur = info.GetDuration();
            Assert.IsTrue(double.IsPositiveInfinity(dur),
                $"Live stream GetDuration() expected +inf, got {dur}");

            // ---- PLAYING ----
            bool playing = false;
            for (int i = 0; i < 1200; i++)
            {
                if (_player.Control.IsPlaying())
                {
                    playing = true;
                    break;
                }
                yield return null;
            }
            Assert.IsTrue(playing, "Never reached PLAYING state on live HLS");

            // ---- decode progress: position must advance on the live wire ----
            double t0 = _player.Control.GetCurrentTime();
            Texture firstTex = _player.TextureProducer.GetTexture();
            bool sawTexture = firstTex != null;

            double t1 = t0;
            for (int i = 0; i < 1200; i++)
            {
                yield return null;
                t1 = _player.Control.GetCurrentTime();
                if (_player.TextureProducer.GetTexture() != null)
                    sawTexture = true;
                if (t1 > t0)
                    break;
            }

            Assert.IsTrue(t1 > t0,
                $"Live playback position did not advance (t0={t0}, t1={t1}) — decode not progressing");

            // ---- texture asserts: GUARDED for -nographics ----
            if (SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.Null)
            {
                Debug.Log("[ObsLiveStreamTest] -nographics: skipping hard texture asserts; " +
                          "live decode verified via metadata + +inf duration + advancing position. " +
                          $"sawTexture={sawTexture}");
            }
            else
            {
                Assert.IsTrue(sawTexture, "Expected a decoded live video texture with a graphics device");
                Texture tex = _player.TextureProducer.GetTexture();
                Assert.AreEqual(ExpectedWidth, tex.width, "live texture width");
                Assert.AreEqual(ExpectedHeight, tex.height, "live texture height");
            }

            _player.CloseMedia();
        }
    }
}
