// SPDX-License-Identifier: Apache-2.0
// Real-GfxDevice pixel readback; self-skips without a GPU, fixture, or decode backend.

using System.Collections;
using System.IO;
using NUnit.Framework;
using UnitedAV;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
using UnityEngine.TestTools;

namespace UnitedAV.Tests.PlayMode
{
    public class PixelReadbackTests
    {
        private MediaPlayer _player;
        private GameObject _go;

        [SetUp]
        public void SetUp()
        {
            _go = new GameObject("UnitedAV_PixelReadback_MediaPlayer");
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

        [UnityTest]
        public IEnumerator DecodedFrame_PixelsAreRealImage_AndHonorTextureContract()
        {
            if (SystemInfo.graphicsDeviceType == GraphicsDeviceType.Null)
                Assert.Ignore("No graphics device (running under -nographics). " +
                              "Pixel-readback test requires a real GfxDevice; skipping.");

            string clip = ResolveH264Clip();
            if (clip == null)
                Assert.Ignore("No H.264 test clip found (set UAV_TEST_MEDIA_DIR or " +
                              "place an *h264* clip under tests/media/out). Skipping pixel-readback test.");

            bool opened = _player.OpenMedia(MediaPathType.AbsolutePathOrURL, clip, autoPlay: true);
            if (!opened)
                Assert.Ignore("Native open failed (plugin/FFmpeg not available in this Unity process). Skipping.");

            Assert.IsTrue(_player.MediaOpened);

            const float timeoutSeconds = 30f;
            float deadline = Time.realtimeSinceStartup + timeoutSeconds;
            Texture tex = null;
            while (Time.realtimeSinceStartup < deadline)
            {
                tex = _player.TextureProducer.GetTexture();
                if (tex != null &&
                    _player.Info.GetVideoWidth() > 0 &&
                    _player.Info.GetVideoHeight() > 0)
                {
                    break;
                }
                yield return null;
            }

            if (tex == null ||
                _player.Info.GetVideoWidth() <= 0 ||
                _player.Info.GetVideoHeight() <= 0)
            {
                Assert.Ignore("No frame was uploaded with valid dimensions (no decode " +
                              "backend in this Unity process). Skipping pixel assertions.");
            }

            int w = _player.Info.GetVideoWidth();
            int h = _player.Info.GetVideoHeight();

            Assert.AreEqual(w, tex.width, "Texture width must match reported video width.");
            Assert.AreEqual(h, tex.height, "Texture height must match reported video height.");

            Assert.IsTrue(_player.TextureProducer.RequiresVerticalFlip(),
                "CPU path frames are top-down; RequiresVerticalFlip() must be true.");
            Assert.IsTrue(GraphicsFormatUtility.IsSRGBFormat(tex.graphicsFormat),
                "Decoded video texture must be sRGB (created with linear:false) so the " +
                "consumer's blit path stays unchanged.");

            Color32[] pixels = null;

            bool asyncSupported = SystemInfo.supportsAsyncGPUReadback;
            if (asyncSupported)
            {
                var req = AsyncGPUReadback.Request(tex, 0, TextureFormat.RGBA32);
                float rbDeadline = Time.realtimeSinceStartup + 10f;
                while (!req.done && Time.realtimeSinceStartup < rbDeadline)
                    yield return null;

                if (req.done && !req.hasError)
                {
                    var data = req.GetData<Color32>();
                    pixels = data.ToArray();
                }
                // On async error/timeout, fall through to the Blit+ReadPixels path.
            }

            if (pixels == null)
                pixels = ReadbackViaRenderTexture(tex, w, h);

            Assert.IsNotNull(pixels, "Pixel readback returned no data on a device that should support it.");
            Assert.AreEqual(w * h, pixels.Length,
                "Readback must return exactly width*height texels.");

            Assert.IsTrue(IsNonConstant(pixels),
                "Decoded frame is a single constant colour — the texture exists but no " +
                "real image was uploaded (blank/black/garbage). Expected spatial variation.");

            Assert.IsTrue(HasOpaquePixel(pixels),
                "Every readback texel is fully transparent — likely a mis-packed RGBA upload.");
        }

        private static Color32[] ReadbackViaRenderTexture(Texture src, int w, int h)
        {
            var desc = new RenderTextureDescriptor(w, h, RenderTextureFormat.ARGB32, 0)
            {
                sRGB = true,
            };
            RenderTexture rt = RenderTexture.GetTemporary(desc);
            RenderTexture prev = RenderTexture.active;
            Texture2D readback = null;
            try
            {
                Graphics.Blit(src, rt);
                RenderTexture.active = rt;

                readback = new Texture2D(w, h, TextureFormat.RGBA32, mipChain: false, linear: false);
                readback.ReadPixels(new Rect(0, 0, w, h), 0, 0, recalculateMipMaps: false);
                readback.Apply(false);

                return readback.GetPixels32();
            }
            finally
            {
                RenderTexture.active = prev;
                RenderTexture.ReleaseTemporary(rt);
                if (readback != null)
                    Object.Destroy(readback);
            }
        }

        private static bool IsNonConstant(Color32[] pixels)
        {
            if (pixels == null || pixels.Length < 2)
                return false;

            Color32 first = pixels[0];
            for (int i = 1; i < pixels.Length; i++)
            {
                Color32 p = pixels[i];
                if (p.r != first.r || p.g != first.g || p.b != first.b || p.a != first.a)
                    return true;
            }
            return false;
        }

        private static bool HasOpaquePixel(Color32[] pixels)
        {
            if (pixels == null)
                return false;
            for (int i = 0; i < pixels.Length; i++)
            {
                if (pixels[i].a >= 250)
                    return true;
            }
            return false;
        }

        private static string ResolveH264Clip()
        {
            string envDir = System.Environment.GetEnvironmentVariable("UAV_TEST_MEDIA_DIR");
            string found = FindH264In(envDir);
            if (found != null)
                return found;

            try
            {
                string probe = Application.dataPath;
                for (int i = 0; i < 6 && !string.IsNullOrEmpty(probe); i++)
                {
                    string candidate = Path.Combine(probe, "tests", "media", "out");
                    found = FindH264In(candidate);
                    if (found != null)
                        return found;
                    probe = Path.GetDirectoryName(probe);
                }
            }
            catch { }

            return null;
        }

        private static readonly string[] PreferredH264Fixtures =
        {
            "mp4__h264__aac.mp4",
            "mov__h264__aac.mov",
            "mpegts__h264__mp3.ts",
        };

        private static string FindH264In(string dir)
        {
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
                return null;

            foreach (var name in PreferredH264Fixtures)
            {
                string p = Path.Combine(dir, name);
                if (File.Exists(p))
                    return p;
            }

            var all = Directory.GetFiles(dir, "*", SearchOption.TopDirectoryOnly);
            System.Array.Sort(all, System.StringComparer.Ordinal);
            foreach (var f in all)
            {
                string lower = Path.GetFileName(f).ToLowerInvariant();
                if (lower.Contains("h264") || lower.Contains("avc"))
                    return f;
            }
            return null;
        }
    }
}
