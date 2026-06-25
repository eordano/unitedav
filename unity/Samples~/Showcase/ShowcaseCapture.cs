// SPDX-License-Identifier: Apache-2.0

using System;
using System.Globalization;
using System.IO;
using UnityEngine;
using Debug = UnityEngine.Debug;

namespace UnitedAV.Samples.Showcase
{
    [DisallowMultipleComponent]
    public sealed class ShowcaseCapture : MonoBehaviour
    {
        private bool _enabled;
        private Camera _cam;
        private RenderTexture _rt;
        private Texture2D _tmp;
        private string _dir;
        private int _w, _h, _fps, _frames, _written;
        private float _warmup, _seconds, _next, _startedAt;
        private bool _done;

        private void Start()
        {
            _enabled = Env("UAV_SHOWCASE_CAPTURE") is string e && (e == "1" || e.Equals("true", StringComparison.OrdinalIgnoreCase));
            if (!_enabled) return;

            _cam = GetComponent<Camera>() ?? Camera.main;
            if (_cam == null) { Debug.LogError("[uav-showcase] capture: no camera"); _enabled = false; return; }

            _w = EnvInt("UAV_SHOWCASE_W", 1280);
            _h = EnvInt("UAV_SHOWCASE_H", 720);
            _fps = Mathf.Max(1, EnvInt("UAV_SHOWCASE_FPS", 12));
            _seconds = EnvFloat("UAV_SHOWCASE_SECONDS", 12f);
            _warmup = EnvFloat("UAV_SHOWCASE_WARMUP", 1.5f);
            _dir = Env("UAV_SHOWCASE_DIR");
            if (string.IsNullOrEmpty(_dir)) _dir = Path.Combine(Application.persistentDataPath, "showcase");

            try { Directory.CreateDirectory(_dir); }
            catch (Exception ex) { Debug.LogError($"[uav-showcase] capture: cannot create {_dir}: {ex.Message}"); _enabled = false; return; }

            _rt = new RenderTexture(_w, _h, 24, RenderTextureFormat.ARGB32) { name = "ShowcaseCaptureRT" };
            _rt.Create();
            _tmp = new Texture2D(_w, _h, TextureFormat.RGB24, false, false);
            _frames = Mathf.CeilToInt(_seconds * _fps);
            _startedAt = Time.time;
            _next = _startedAt + _warmup;
            Debug.Log($"[uav-showcase] capture ON -> {_dir} ({_w}x{_h}, {_fps}fps, {_seconds}s = {_frames} frames, warmup {_warmup}s)");
        }

        private void LateUpdate()
        {
            if (!_enabled || _done) return;
            if (Time.time < _next) return;
            _next += 1f / _fps;

            try
            {
                var prevTarget = _cam.targetTexture;
                var prevActive = RenderTexture.active;
                _cam.targetTexture = _rt;
                _cam.Render();
                RenderTexture.active = _rt;
                _tmp.ReadPixels(new Rect(0, 0, _w, _h), 0, 0);
                _tmp.Apply(false);
                _cam.targetTexture = prevTarget;
                RenderTexture.active = prevActive;

                string path = Path.Combine(_dir, $"frame-{_written:00000}.png");
                File.WriteAllBytes(path, _tmp.EncodeToPNG());
                _written++;
            }
            catch (Exception ex)
            {
                Debug.LogError($"[uav-showcase] capture frame {_written} failed: {ex.Message}");
            }

            if (_written >= _frames)
            {
                _done = true;
                Debug.Log($"[uav-showcase] capture DONE: {_written} frames in {_dir}");
                Application.Quit(0);
#if UNITY_EDITOR
                UnityEditor.EditorApplication.isPlaying = false;
#endif
            }
        }

        private static string Env(string k) => Environment.GetEnvironmentVariable(k);
        private static int EnvInt(string k, int d) => int.TryParse(Env(k), NumberStyles.Integer, CultureInfo.InvariantCulture, out var v) ? v : d;
        private static float EnvFloat(string k, float d) => float.TryParse(Env(k), NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : d;
    }
}
