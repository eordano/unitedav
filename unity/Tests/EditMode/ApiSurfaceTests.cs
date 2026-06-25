// SPDX-License-Identifier: Apache-2.0
// UnitedAV — EditMode API-surface tests.
//
// Pure managed reflection/compile assertions that the UnitedAV surface
// (UnitedAV) and the UnitedAV.Streaming sender exist with the
// signatures consumers depend on. These do NOT touch the
// native plugin, so they run in EditMode with no graphics device.

using System;
using System.Reflection;
using System.Runtime.InteropServices;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.Events;
using UnitedAV;
using UnitedAV.Streaming;

namespace UnitedAV.Tests.EditMode
{
    public class ApiSurfaceTests
    {
        // ---------- Namespace / type existence ----------

        [Test]
        public void MediaPlayer_Is_MonoBehaviour_In_UnitedAV_Namespace()
        {
            Type t = typeof(MediaPlayer);
            Assert.AreEqual("UnitedAV", t.Namespace);
            Assert.IsTrue(typeof(MonoBehaviour).IsAssignableFrom(t),
                "MediaPlayer must be a MonoBehaviour (consumers add it as a component).");
        }

        [Test]
        public void MediaPlayer_Exposes_Control_Info_TextureProducer_Events()
        {
            Type t = typeof(MediaPlayer);

            PropertyInfo control = t.GetProperty("Control");
            Assert.NotNull(control, "MediaPlayer.Control missing");
            Assert.AreEqual(typeof(IMediaControl), control.PropertyType);

            PropertyInfo info = t.GetProperty("Info");
            Assert.NotNull(info, "MediaPlayer.Info missing");
            Assert.AreEqual(typeof(IMediaInfo), info.PropertyType);

            PropertyInfo tp = t.GetProperty("TextureProducer");
            Assert.NotNull(tp, "MediaPlayer.TextureProducer missing");
            Assert.AreEqual(typeof(ITextureProducer), tp.PropertyType);

            PropertyInfo events = t.GetProperty("Events");
            Assert.NotNull(events, "MediaPlayer.Events missing");
            Assert.AreEqual(typeof(MediaPlayerEvent), events.PropertyType);
        }

        [Test]
        public void MediaPlayer_OpenMedia_Has_Expected_Signature()
        {
            MethodInfo open = typeof(MediaPlayer).GetMethod(
                "OpenMedia",
                new[] { typeof(MediaPathType), typeof(string), typeof(bool) });
            Assert.NotNull(open, "OpenMedia(MediaPathType, string, bool) missing");
            Assert.AreEqual(typeof(bool), open.ReturnType, "OpenMedia must return bool");
        }

        // ---------- IMediaControl ----------

        [Test]
        public void IMediaControl_Has_Expected_Members()
        {
            Type t = typeof(IMediaControl);
            AssertMethod(t, "Play", typeof(bool));
            AssertMethod(t, "Pause", typeof(bool));
            AssertMethod(t, "Stop", typeof(bool));
            AssertMethod(t, "Seek", typeof(bool), typeof(double));
            AssertMethod(t, "IsPlaying", typeof(bool));
            AssertMethod(t, "IsPaused", typeof(bool));
            AssertMethod(t, "IsFinished", typeof(bool));
            AssertMethod(t, "GetCurrentTime", typeof(double));
            AssertMethod(t, "GetLastError", typeof(ErrorCode));
            AssertMethod(t, "GetBufferedTimes", typeof(TimeRanges));
        }

        // ---------- IMediaInfo ----------

        [Test]
        public void IMediaInfo_Has_Expected_Members()
        {
            Type t = typeof(IMediaInfo);
            AssertMethod(t, "GetDuration", typeof(double));
            AssertMethod(t, "GetVideoWidth", typeof(int));
            AssertMethod(t, "GetVideoHeight", typeof(int));
            AssertMethod(t, "GetVideoFrameRate", typeof(float));
            AssertMethod(t, "HasVideo", typeof(bool));
            AssertMethod(t, "HasAudio", typeof(bool));
        }

        // ---------- ITextureProducer ----------

        [Test]
        public void ITextureProducer_Has_Expected_Members()
        {
            Type t = typeof(ITextureProducer);
            AssertMethod(t, "GetTexture", typeof(Texture));
            AssertMethod(t, "RequiresVerticalFlip", typeof(bool));
        }

        // ---------- MediaPlayerEvent.EventType ----------

        [Test]
        public void MediaPlayerEvent_Derives_From_3Arg_UnityEvent()
        {
            Type baseType = typeof(UnityEvent<MediaPlayer, MediaPlayerEvent.EventType, ErrorCode>);
            Assert.IsTrue(baseType.IsAssignableFrom(typeof(MediaPlayerEvent)),
                "MediaPlayerEvent must derive from UnityEvent<MediaPlayer, EventType, ErrorCode>");
        }

        [Test]
        public void MediaPlayerEvent_EventType_Has_Core_Members()
        {
            Type et = typeof(MediaPlayerEvent.EventType);
            Assert.IsTrue(et.IsEnum);
            foreach (string name in new[]
            {
                "MetaDataReady", "ReadyToPlay", "Started", "FirstFrameReady",
                "FinishedPlaying", "Closing", "Error", "ResolutionChanged",
            })
            {
                Assert.IsTrue(Enum.IsDefined(et, name),
                    $"MediaPlayerEvent.EventType.{name} missing");
            }
        }

        // ---------- MediaPathType / ErrorCode values ----------

        [Test]
        public void MediaPathType_Values_Match_UnitedAV()
        {
            Assert.AreEqual(0, (int)MediaPathType.AbsolutePathOrURL);
            Assert.AreEqual(1, (int)MediaPathType.RelativeToProjectFolder);
            Assert.AreEqual(2, (int)MediaPathType.RelativeToStreamingAssetsFolder);
            Assert.AreEqual(3, (int)MediaPathType.RelativeToDataFolder);
            Assert.AreEqual(4, (int)MediaPathType.RelativeToPersistentDataFolder);
        }

        [Test]
        public void ErrorCode_Has_None_Zero()
        {
            Assert.AreEqual(0, (int)ErrorCode.None);
            Assert.IsTrue(Enum.IsDefined(typeof(ErrorCode), "None"));
        }

        // ---------- UnitedAV.Streaming sender surface ----------

        [Test]
        public void MediaSender_Exists_With_Expected_Methods()
        {
            Type t = typeof(MediaSender);
            Assert.AreEqual("UnitedAV.Streaming", t.Namespace);
            Assert.IsTrue(typeof(IDisposable).IsAssignableFrom(t), "MediaSender must be IDisposable");

            MethodInfo open = t.GetMethod("Open", new[] { typeof(string), typeof(UAVSendConfig) });
            Assert.NotNull(open, "MediaSender.Open(string, UAVSendConfig) missing");
            Assert.AreEqual(typeof(UAVSendResult), open.ReturnType);

            MethodInfo pushVideo = t.GetMethod("PushVideo",
                new[] { typeof(byte[]), typeof(int), typeof(int), typeof(int), typeof(double) });
            Assert.NotNull(pushVideo, "MediaSender.PushVideo(byte[],int,int,int,double) missing");
            Assert.AreEqual(typeof(UAVSendResult), pushVideo.ReturnType);

            MethodInfo pushAudio = t.GetMethod("PushAudio",
                new[] { typeof(float[]), typeof(int), typeof(int), typeof(int), typeof(double) });
            Assert.NotNull(pushAudio, "MediaSender.PushAudio(float[],int,int,int,double) missing");
            Assert.AreEqual(typeof(UAVSendResult), pushAudio.ReturnType);

            MethodInfo close = t.GetMethod("Close", Type.EmptyTypes);
            Assert.NotNull(close, "MediaSender.Close() missing");
            Assert.AreEqual(typeof(UAVSendResult), close.ReturnType);
        }

        [Test]
        public void UAVSendConfig_Is_Sequential_With_Nine_Int_Fields()
        {
            Type t = typeof(UAVSendConfig);
            Assert.IsTrue(t.IsValueType, "UAVSendConfig must be a struct");

            // Layout must be Sequential so it marshals 1:1 with the C header.
            StructLayoutAttribute layout = t.StructLayoutAttribute;
            Assert.NotNull(layout);
            Assert.AreEqual(LayoutKind.Sequential, layout.Value);

            // Exactly the nine int32 fields, in header order.
            string[] expected =
            {
                "video_codec", "width", "height", "fps", "video_bitrate",
                "audio_codec", "sample_rate", "channels", "audio_bitrate",
            };
            FieldInfo[] fields = t.GetFields(BindingFlags.Public | BindingFlags.Instance);
            Assert.AreEqual(expected.Length, fields.Length,
                "UAVSendConfig must have exactly 9 instance fields");
            for (int i = 0; i < expected.Length; i++)
            {
                Assert.AreEqual(expected[i], fields[i].Name, $"field #{i} order mismatch");
                Assert.AreEqual(typeof(int), fields[i].FieldType, $"field {fields[i].Name} must be int32");
            }

            // Marshal size must equal 9 * 4 bytes (no padding/extra fields).
            Assert.AreEqual(9 * sizeof(int), Marshal.SizeOf<UAVSendConfig>(),
                "UAVSendConfig marshal size must be 36 bytes");
        }

        [Test]
        public void UAVSendConfig_Default_Picks_VP9_Opus()
        {
            UAVSendConfig cfg = UAVSendConfig.Default();
            Assert.AreEqual((int)UAVVideoCodec.VP9, cfg.video_codec);
            Assert.AreEqual((int)UAVAudioCodec.Opus, cfg.audio_codec);
        }

        [Test]
        public void Sender_Enums_Match_Header_Values()
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

        // ---------- helpers ----------

        private static void AssertMethod(Type t, string name, Type returnType, params Type[] args)
        {
            MethodInfo m = t.GetMethod(name, args ?? Type.EmptyTypes);
            Assert.NotNull(m, $"{t.Name}.{name}({string.Join(",", Array.ConvertAll(args ?? Type.EmptyTypes, a => a.Name))}) missing");
            Assert.AreEqual(returnType, m.ReturnType, $"{t.Name}.{name} return type mismatch");
        }
    }
}
