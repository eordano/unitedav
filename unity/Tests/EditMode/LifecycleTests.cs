// SPDX-License-Identifier: Apache-2.0

using NUnit.Framework;
using UnitedAV;
using UnitedAV.Streaming;
using UnitedAV.Windows;
using UnityEngine.Events;

namespace UnitedAV.Tests.EditMode
{
    public class LifecycleTests
    {
        [Test]
        public void NewEvent_HasNoListeners()
        {
            var ev = new MediaPlayerEvent();
            Assert.IsFalse(ev.HasListeners(),
                "A freshly constructed MediaPlayerEvent must report no listeners.");
        }

        [Test]
        public void AddListener_MakesHasListenersTrue()
        {
            var ev = new MediaPlayerEvent();
            UnityAction<MediaPlayer, MediaPlayerEvent.EventType, ErrorCode> cb =
                (mp, et, code) => { };

            ev.AddListener(cb);

            Assert.IsTrue(ev.HasListeners(),
                "HasListeners() must be true after a runtime AddListener.");
        }

        [Test]
        public void RemoveListener_ClearsHasListeners()
        {
            var ev = new MediaPlayerEvent();
            UnityAction<MediaPlayer, MediaPlayerEvent.EventType, ErrorCode> cb =
                (mp, et, code) => { };

            ev.AddListener(cb);
            ev.RemoveListener(cb);

            Assert.IsFalse(ev.HasListeners(),
                "Removing the only runtime listener must clear HasListeners().");
        }

        [Test]
        public void RemoveAllListeners_ClearsHasListeners()
        {
            var ev = new MediaPlayerEvent();
            ev.AddListener((mp, et, code) => { });
            ev.AddListener((mp, et, code) => { });

            ev.RemoveAllListeners();

            Assert.IsFalse(ev.HasListeners(),
                "RemoveAllListeners() must reset the runtime listener bookkeeping.");
        }

        [Test]
        public void RemoveListener_BelowZero_DoesNotUnderflow()
        {
            var ev = new MediaPlayerEvent();
            UnityAction<MediaPlayer, MediaPlayerEvent.EventType, ErrorCode> cb =
                (mp, et, code) => { };

            ev.RemoveListener(cb);
            ev.RemoveListener(cb);
            ev.AddListener(cb);

            Assert.IsTrue(ev.HasListeners(),
                "A subscribed listener must report HasListeners() even after prior over-removals (count must not underflow below zero).");
        }

        [Test]
        public void Invoke_DeliversPlayerEventTypeAndCode()
        {
            var ev = new MediaPlayerEvent();
            int calls = 0;
            MediaPlayerEvent.EventType seenType = default;
            ErrorCode seenCode = ErrorCode.None;

            ev.AddListener((mp, et, code) =>
            {
                calls++;
                seenType = et;
                seenCode = code;
            });

            ev.Invoke(null, MediaPlayerEvent.EventType.Error, ErrorCode.DecodeFailed);

            Assert.AreEqual(1, calls, "Listener must be invoked exactly once.");
            Assert.AreEqual(MediaPlayerEvent.EventType.Error, seenType);
            Assert.AreEqual(ErrorCode.DecodeFailed, seenCode);
        }

        // These pin the public managed enums to the frozen C ABI integer values.
        [Test]
        public void SendCodecAndResult_Values_MatchFrozenSendAbi()
        {
            Assert.AreEqual(0, (int)UAVVideoCodec.None);
            Assert.AreEqual(1, (int)UAVVideoCodec.VP9);
            Assert.AreEqual(2, (int)UAVVideoCodec.VP8);
            Assert.AreEqual(3, (int)UAVVideoCodec.AV1);

            Assert.AreEqual(0, (int)UAVAudioCodec.None);
            Assert.AreEqual(1, (int)UAVAudioCodec.Opus);

            Assert.AreEqual(0, (int)UAVSendResult.Ok);
            Assert.AreEqual(-1, (int)UAVSendResult.ErrInvalid);
            Assert.AreEqual(-2, (int)UAVSendResult.ErrOpenFailed);
            Assert.AreEqual(-3, (int)UAVSendResult.ErrNoStream);
            Assert.AreEqual(-4, (int)UAVSendResult.ErrEncode);
            Assert.AreEqual(-5, (int)UAVSendResult.ErrUnsupported);
            Assert.AreEqual(-6, (int)UAVSendResult.ErrNoMem);
        }

        [Test]
        public void MediaPathType_Values_MatchSpec()
        {
            Assert.AreEqual(0, (int)MediaPathType.AbsolutePathOrURL);
            Assert.AreEqual(1, (int)MediaPathType.RelativeToProjectFolder);
            Assert.AreEqual(2, (int)MediaPathType.RelativeToStreamingAssetsFolder);
            Assert.AreEqual(3, (int)MediaPathType.RelativeToDataFolder);
            Assert.AreEqual(4, (int)MediaPathType.RelativeToPersistentDataFolder);
        }

        [Test]
        public void WindowsEnums_Values_MatchSpec()
        {
            Assert.AreEqual(0, (int)VideoApi.WinRT);
            Assert.AreEqual(1, (int)VideoApi.MediaFoundation);
            Assert.AreEqual(2, (int)VideoApi.DirectShow);

            Assert.AreEqual(0, (int)AudioOutput.System);
            Assert.AreEqual(1, (int)AudioOutput.Unity);
            Assert.AreEqual(2, (int)AudioOutput.FacebookAudio360);
        }

        [Test]
        public void AudioModeEnum_Values_MatchSpec()
        {
            Assert.AreEqual(0, (int)MediaPlayer.PlatformOptions.AudioMode.SystemDirect);
            Assert.AreEqual(1, (int)MediaPlayer.PlatformOptions.AudioMode.Unity);
            Assert.AreEqual(2, (int)MediaPlayer.PlatformOptions.AudioMode.FacebookAudio360);
        }
    }
}
