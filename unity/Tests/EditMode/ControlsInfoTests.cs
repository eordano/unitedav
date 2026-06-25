// SPDX-License-Identifier: Apache-2.0

using NUnit.Framework;
using UnitedAV;
using UnitedAV.Streaming;
using UnitedAV.Windows;

namespace UnitedAV.Tests.EditMode
{
    public class ControlsInfoTests
    {
        [Test]
        public void TimeRange_EndTime_IsStartPlusDuration()
        {
            var r = new TimeRange(2.0, 3.5);
            Assert.AreEqual(2.0, r.startTime, 1e-9);
            Assert.AreEqual(3.5, r.duration, 1e-9);
            Assert.AreEqual(5.5, r.EndTime, 1e-9);
        }

        [Test]
        public void TimeRanges_Empty_HasZeroCount()
        {
            var ranges = new TimeRanges();
            Assert.AreEqual(0, ranges.Count);
            Assert.AreEqual(0, ranges.Ranges.Count);
        }

        [Test]
        public void TimeRanges_NullList_IsTreatedAsEmpty()
        {
            var ranges = new TimeRanges(null);
            Assert.AreEqual(0, ranges.Count);
        }

        [Test]
        public void TimeRanges_FromList_ExposesCountAndIndexer()
        {
            var list = new System.Collections.Generic.List<TimeRange>
            {
                new TimeRange(0.0, 1.0),
                new TimeRange(2.0, 0.5),
            };
            var ranges = new TimeRanges(list);

            Assert.AreEqual(2, ranges.Count);
            Assert.AreEqual(0.0, ranges[0].startTime, 1e-9);
            Assert.AreEqual(2.5, ranges[1].EndTime, 1e-9);
            Assert.AreEqual(2, ranges.Ranges.Count);
        }

        [Test]
        public void SendConfigDefault_IsVp9PlusOpus()
        {
            var cfg = UAVSendConfig.Default();

            Assert.AreEqual((int)UAVVideoCodec.VP9, cfg.video_codec);
            Assert.AreEqual((int)UAVAudioCodec.Opus, cfg.audio_codec);
            Assert.AreEqual(0, cfg.width,  "width 0 => taken from first pushed frame");
            Assert.AreEqual(0, cfg.height, "height 0 => taken from first pushed frame");
            Assert.AreEqual(30, cfg.fps);
            Assert.AreEqual(0, cfg.video_bitrate, "0 => native default");
            Assert.AreEqual(48000, cfg.sample_rate);
            Assert.AreEqual(2, cfg.channels);
            Assert.AreEqual(0, cfg.audio_bitrate, "0 => native default");
        }

        [Test]
        public void SendConfig_FieldsAreAssignable()
        {
            // Touches every field so a renamed/reordered field breaks the build.
            var cfg = new UAVSendConfig
            {
                video_codec = (int)UAVVideoCodec.AV1,
                width = 1280,
                height = 720,
                fps = 60,
                video_bitrate = 4_000_000,
                audio_codec = (int)UAVAudioCodec.Opus,
                sample_rate = 44100,
                channels = 1,
                audio_bitrate = 64000,
            };

            Assert.AreEqual((int)UAVVideoCodec.AV1, cfg.video_codec);
            Assert.AreEqual(1280, cfg.width);
            Assert.AreEqual(720, cfg.height);
            Assert.AreEqual(60, cfg.fps);
            Assert.AreEqual(4_000_000, cfg.video_bitrate);
            Assert.AreEqual(44100, cfg.sample_rate);
            Assert.AreEqual(1, cfg.channels);
            Assert.AreEqual(64000, cfg.audio_bitrate);
        }

        [Test]
        public void PlatformOptionsWindows_Defaults()
        {
            var o = new PlatformOptionsWindows();
            Assert.AreEqual(VideoApi.WinRT, o.videoApi);
            Assert.AreEqual(AudioOutput.System, o._audioMode);
            Assert.IsFalse(o.startWithHighestBitrate);
            Assert.IsFalse(o.useLowLiveLatency);
        }

        [Test]
        public void PlatformOptionsMacOS_Defaults()
        {
            var o = new PlatformOptions_macOS();
            Assert.AreEqual(MediaPlayer.PlatformOptions.AudioMode.SystemDirect, o.audioMode);
        }

        [Test]
        public void ErrorCode_NoneIsZero()
        {
            Assert.AreEqual(0, (int)ErrorCode.None);
        }
    }
}
