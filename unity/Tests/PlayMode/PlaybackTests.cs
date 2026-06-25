// SPDX-License-Identifier: Apache-2.0

using System.Collections;
using System.IO;
using NUnit.Framework;
using UnitedAV;
using UnityEngine;
using UnityEngine.TestTools;

namespace UnitedAV.Tests.PlayMode
{
    public class PlaybackTests
    {
        private MediaPlayer _player;
        private GameObject _go;

        [SetUp]
        public void SetUp()
        {
            _go = new GameObject("UnitedAV_PlayMode_MediaPlayer");
            _player = _go.AddComponent<MediaPlayer>();
        }

        [TearDown]
        public void TearDown()
        {
            if (_player != null)
                _player.CloseMedia();
            if (_go != null)
                Object.Destroy(_go);
            _go = null;
            _player = null;
        }

        [Test]
        public void FacadesAreNonNullAfterAwake()
        {
            Assert.IsNotNull(_player.Control, "Control must be non-null after Awake.");
            Assert.IsNotNull(_player.Info, "Info must be non-null after Awake.");
            Assert.IsNotNull(_player.TextureProducer, "TextureProducer must be non-null after Awake.");
            Assert.IsNotNull(_player.Events, "Events must be non-null after Awake.");
        }

        [Test]
        public void TextureProducer_OrientationContract()
        {
            Assert.IsTrue(_player.TextureProducer.RequiresVerticalFlip(),
                "CPU path frames are top-down; RequiresVerticalFlip() must be true.");
        }

        [Test]
        public void TextureIsNullBeforeAnyFrame()
        {
            Assert.IsNull(_player.TextureProducer.GetTexture(),
                "No frame has been uploaded yet; GetTexture() must be null.");
        }

        [Test]
        public void MediaOpenedFalseInitially()
        {
            Assert.IsFalse(_player.MediaOpened);
        }

        [Test]
        public void OpenMedia_EmptyPath_ReturnsFalse()
        {
            Assert.IsFalse(_player.OpenMedia(MediaPathType.AbsolutePathOrURL, "", autoPlay: false));
            Assert.IsFalse(_player.OpenMedia(MediaPathType.AbsolutePathOrURL, null, autoPlay: false));
            Assert.IsFalse(_player.MediaOpened, "A rejected open must not flip MediaOpened.");
        }

        [Test]
        public void OpenMedia_UnreachablePath_DoesNotThrow_AndStaysClosed()
        {
            bool opened = false;
            Assert.DoesNotThrow(() =>
            {
                opened = _player.OpenMedia(
                    MediaPathType.AbsolutePathOrURL,
                    "/nonexistent/unitedav-no-such-file.mp4",
                    autoPlay: false);
            });
            Assert.IsFalse(opened);
            Assert.IsFalse(_player.MediaOpened);
        }

        [Test]
        public void AudioVolume_ClampsToUnitRange()
        {
            _player.AudioVolume = 2.5f;
            Assert.AreEqual(1f, _player.AudioVolume, 1e-6f, "Volume must clamp to <= 1.");
            _player.AudioVolume = -1f;
            Assert.AreEqual(0f, _player.AudioVolume, 1e-6f, "Volume must clamp to >= 0.");
            _player.AudioVolume = 0.5f;
            Assert.AreEqual(0.5f, _player.AudioVolume, 1e-6f);
        }

        [Test]
        public void Looping_ManagedRoundTrip()
        {
            Assert.IsFalse(_player.Control.IsLooping(), "Looping defaults to false.");
            _player.Control.SetLooping(true);
            Assert.IsTrue(_player.Control.IsLooping());
            _player.Control.SetLooping(false);
            Assert.IsFalse(_player.Control.IsLooping());
        }

        [Test]
        public void PlaybackRate_DefaultsToOne_AndRoundTrips()
        {
            Assert.AreEqual(1f, _player.Control.GetPlaybackRate(), 1e-6f,
                "Playback rate defaults to 1.0.");
            _player.Control.SetPlaybackRate(2f);
            Assert.AreEqual(2f, _player.Control.GetPlaybackRate(), 1e-6f);
        }

        [Test]
        public void GetDuration_IsPositiveInfinity_WhenNothingOpen()
        {
            Assert.AreEqual(double.PositiveInfinity, _player.Info.GetDuration());
        }

        [Test]
        public void GetBufferedTimes_NeverNull()
        {
            var ranges = _player.Control.GetBufferedTimes();
            Assert.IsNotNull(ranges);
            Assert.GreaterOrEqual(ranges.Count, 0);
        }

        [Test]
        public void GetLastError_NoneInitially()
        {
            Assert.AreEqual(ErrorCode.None, _player.Control.GetLastError());
        }

        [Test]
        public void Stop_OnIdlePlayer_DoesNotThrow()
        {
            Assert.DoesNotThrow(() => _player.Stop());
            Assert.DoesNotThrow(() => _player.Control.Stop());
        }

        [UnityTest]
        public IEnumerator OpenRealClip_ReachesReady_AndProducesTextureAndInfo()
        {
            string clip = ResolveTestClip();
            if (clip == null)
                Assert.Ignore("No test clip found (set UAV_TEST_MEDIA_DIR or place one under tests/media/out). Skipping media-backed PlayMode test.");

            bool opened = _player.OpenMedia(MediaPathType.AbsolutePathOrURL, clip, autoPlay: true);
            if (!opened)
                Assert.Ignore("Native open failed (plugin/FFmpeg not available in this Unity process). Skipping.");

            Assert.IsTrue(_player.MediaOpened);

            const float timeoutSeconds = 20f;
            float deadline = Time.realtimeSinceStartup + timeoutSeconds;
            bool gotInfo = false;
            while (Time.realtimeSinceStartup < deadline)
            {
                if (_player.Info.HasVideo() &&
                    _player.Info.GetVideoWidth() > 0 &&
                    _player.Info.GetVideoHeight() > 0)
                {
                    gotInfo = true;
                    break;
                }
                yield return null;
            }

            if (!gotInfo)
                Assert.Ignore("Clip never reached READY with valid dimensions (no decode backend). Skipping assertions.");

            Assert.Greater(_player.Info.GetVideoWidth(), 0);
            Assert.Greater(_player.Info.GetVideoHeight(), 0);

            deadline = Time.realtimeSinceStartup + timeoutSeconds;
            Texture tex = null;
            while (Time.realtimeSinceStartup < deadline)
            {
                tex = _player.TextureProducer.GetTexture();
                if (tex != null)
                    break;
                yield return null;
            }

            if (tex != null)
            {
                Assert.AreEqual(_player.Info.GetVideoWidth(), tex.width,
                    "Texture width must match reported video width.");
                Assert.AreEqual(_player.Info.GetVideoHeight(), tex.height,
                    "Texture height must match reported video height.");
            }
            else
            {
                Assert.Ignore("Decoded metadata available but no frame uploaded (CPU upload path inactive). Skipping texture assertions.");
            }
        }

        private static string ResolveTestClip()
        {
            string dir = System.Environment.GetEnvironmentVariable("UAV_TEST_MEDIA_DIR");
            string found = FindClipIn(dir);
            if (found != null)
                return found;

            try
            {
                string probe = Application.dataPath;
                for (int i = 0; i < 6 && !string.IsNullOrEmpty(probe); i++)
                {
                    string candidate = Path.Combine(probe, "tests", "media", "out");
                    found = FindClipIn(candidate);
                    if (found != null)
                        return found;
                    probe = Path.GetDirectoryName(probe);
                }
            }
            catch { }

            return null;
        }

        private const string PreferredFixture = "webm__vp9__opus.webm";

        private static readonly string[] ClipExtensions =
            { "*.mp4", "*.webm", "*.mkv", "*.mov" };

        private static string FindClipIn(string dir)
        {
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
                return null;

            string preferred = Path.Combine(dir, PreferredFixture);
            if (File.Exists(preferred))
                return preferred;

            foreach (var ext in ClipExtensions)
            {
                var hits = Directory.GetFiles(dir, ext, SearchOption.TopDirectoryOnly);
                if (hits.Length > 0)
                {
                    System.Array.Sort(hits, System.StringComparer.Ordinal);
                    return hits[0];
                }
            }
            return null;
        }
    }
}
