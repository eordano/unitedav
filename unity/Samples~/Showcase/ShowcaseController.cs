// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using UnityEngine;
using UnitedAV;
using Debug = UnityEngine.Debug;

namespace UnitedAV.Samples.Showcase
{
    [DisallowMultipleComponent]
    public sealed class ShowcaseController : MonoBehaviour
    {
        [Header("Gallery layout")]
        [Tooltip("How many video screens to arrange around the ring.")]
        public int screenCount = 8;
        [Tooltip("Ring radius (screens sit on this circle, facing inward).")]
        public float ringRadius = 7.0f;
        [Tooltip("Screen height (world units); width follows the clip's aspect.")]
        public float screenHeight = 2.6f;
        [Tooltip("Vertical center of the ring of screens.")]
        public float screenElevation = 1.6f;

        [Header("Media")]
        [Tooltip("Optional explicit folder of clips. Else UAV_TEST_MEDIA_DIR / tests/media/out.")]
        public string mediaDirOverride = "";

        private readonly List<Screen> _screens = new List<Screen>();

        private sealed class Screen
        {
            public MediaPlayer player;
            public Renderer renderer;
            public Material mat;
            public string clip;
            public bool flipApplied;
        }

        private void Start()
        {
            Application.runInBackground = true;
            QualitySettings.vSyncCount = 0;
            Application.targetFrameRate = 60;

            BuildEnvironment();

            var clips = ResolveClips();
            if (clips.Count == 0)
            {
                Debug.LogError("[uav-showcase] no video clips found (set UAV_TEST_MEDIA_DIR or run tests/media/gen.sh). Showing an empty gallery.");
            }

            int n = Mathf.Max(1, screenCount);
            for (int i = 0; i < n; i++)
            {
                float ang = (i / (float)n) * Mathf.PI * 2f;
                var pos = new Vector3(Mathf.Sin(ang) * ringRadius, screenElevation, Mathf.Cos(ang) * ringRadius);
                string clip = clips.Count > 0 ? clips[i % clips.Count] : null;
                _screens.Add(BuildScreen(i, pos, clip));
            }

            var camGo = new GameObject("ShowcaseCamera");
            var cam = camGo.AddComponent<Camera>();
            cam.backgroundColor = new Color(0.02f, 0.02f, 0.035f);
            cam.clearFlags = CameraClearFlags.SolidColor;
            cam.fieldOfView = 60f;
            cam.nearClipPlane = 0.05f;
            cam.farClipPlane = 200f;
            var orbit = camGo.AddComponent<OrbitCamera>();
            orbit.target = new Vector3(0f, screenElevation, 0f);
            orbit.radius = ringRadius + 4.5f;
            orbit.height = screenElevation + 2.2f;

            camGo.AddComponent<ShowcaseCapture>();

            Debug.Log($"[uav-showcase] built {_screens.Count} screens, {clips.Count} distinct clips, orbiting camera up.");
        }

        private void Update()
        {
            foreach (var s in _screens)
            {
                if (s == null || s.player == null || s.renderer == null) continue;
                Texture tex = null;
                try { tex = s.player.TextureProducer != null ? s.player.TextureProducer.GetTexture() : null; }
                catch { tex = null; }
                if (tex == null) continue;

                if (s.mat.mainTexture != tex)
                    s.mat.mainTexture = tex;

                if (!s.flipApplied)
                {
                    bool flip = false;
                    try { flip = s.player.TextureProducer != null && s.player.TextureProducer.RequiresVerticalFlip(); }
                    catch { flip = false; }
                    if (flip) { s.mat.mainTextureScale = new Vector2(1f, -1f); s.mat.mainTextureOffset = new Vector2(0f, 1f); }
                    s.flipApplied = true;
                }
            }
        }

        private Screen BuildScreen(int index, Vector3 pos, string clip)
        {
            var quad = GameObject.CreatePrimitive(PrimitiveType.Quad);
            quad.name = $"Screen{index}";
            DropCollider(quad);
            quad.transform.position = pos;
            quad.transform.rotation = Quaternion.LookRotation((quad.transform.position - new Vector3(0f, pos.y, 0f)).normalized);
            quad.transform.localScale = new Vector3(screenHeight * (16f / 9f), screenHeight, 1f);

            var mat = new Material(FindUnlitShader());
            mat.color = Color.white;
            mat.mainTexture = Texture2D.blackTexture;
            quad.GetComponent<Renderer>().material = mat;

            var bezel = GameObject.CreatePrimitive(PrimitiveType.Quad);
            bezel.name = $"Bezel{index}";
            DropCollider(bezel);
            bezel.transform.SetParent(quad.transform, false);
            bezel.transform.localPosition = new Vector3(0f, 0f, 0.02f);
            bezel.transform.localScale = new Vector3(1.06f, 1.10f, 1f);
            var bmat = new Material(FindUnlitShader());
            bmat.mainTexture = SolidTex(new Color(0.05f, 0.05f, 0.06f));
            bezel.GetComponent<Renderer>().material = bmat;

            var s = new Screen { renderer = quad.GetComponent<Renderer>(), mat = mat, clip = clip };

            if (!string.IsNullOrEmpty(clip))
            {
                var mp = quad.AddComponent<MediaPlayer>();
                mp.AutoOpen = false;
                s.player = mp;
                bool opened = false;
                try { opened = mp.OpenMedia(MediaPathType.AbsolutePathOrURL, clip, autoPlay: true); }
                catch (Exception e) { Debug.LogWarning($"[uav-showcase] open failed for {Path.GetFileName(clip)}: {e.Message}"); }
                if (opened)
                {
                    try { mp.Control.SetLooping(true); } catch { }
                    try { mp.AudioVolume = 0f; } catch { }
                    try { mp.Control.Play(); } catch { }
                }
            }
            return s;
        }

        private void BuildEnvironment()
        {
            var floor = GameObject.CreatePrimitive(PrimitiveType.Plane);
            floor.name = "Floor";
            DropCollider(floor);
            floor.transform.position = new Vector3(0f, 0f, 0f);
            floor.transform.localScale = new Vector3(6f, 1f, 6f);
            var fmat = new Material(FindUnlitShader());
            fmat.mainTexture = SolidTex(new Color(0.03f, 0.03f, 0.05f));
            floor.GetComponent<Renderer>().material = fmat;

            var lightGo = new GameObject("Fill");
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            light.intensity = 0.6f;
            light.color = new Color(0.7f, 0.8f, 1f);
            lightGo.transform.rotation = Quaternion.Euler(55f, -30f, 0f);
            RenderSettings.ambientLight = new Color(0.10f, 0.10f, 0.14f);
        }

        private static Shader FindUnlitShader()
        {
            foreach (var name in new[] { "Unlit/Texture", "Universal Render Pipeline/Unlit", "Sprites/Default" })
            {
                var sh = Shader.Find(name);
                if (sh != null) return sh;
            }
            return Shader.Find("Standard");
        }

        // No hard Collider-type reference: compiles where the physics module is stripped.
        private static void DropCollider(GameObject go)
        {
            var col = go.GetComponent("Collider");
            if (col != null) Destroy(col);
        }

        private static Texture2D SolidTex(Color c)
        {
            var t = new Texture2D(1, 1, TextureFormat.RGBA32, false);
            t.SetPixel(0, 0, c);
            t.Apply(false);
            return t;
        }

        private static readonly string[] VideoExts = { "*.mp4", "*.webm", "*.mkv", "*.mov", "*.ts" };

        private List<string> ResolveClips()
        {
            var clips = new List<string>();
            foreach (var dir in CandidateDirs())
            {
                foreach (var ext in VideoExts)
                {
                    string[] hits;
                    try { hits = Directory.GetFiles(dir, ext, SearchOption.TopDirectoryOnly); }
                    catch { continue; }
                    foreach (var h in hits)
                    {
                        if (h.IndexOf("novideo", StringComparison.OrdinalIgnoreCase) >= 0) continue;
                        clips.Add(h);
                    }
                }
                if (clips.Count > 0) break;
            }
            clips.Sort(StringComparer.Ordinal);
            var seen = new HashSet<string>();
            var unique = new List<string>();
            foreach (var c in clips) if (seen.Add(c)) unique.Add(c);
            return unique;
        }

        private IEnumerable<string> CandidateDirs()
        {
            if (!string.IsNullOrEmpty(mediaDirOverride) && Directory.Exists(mediaDirOverride))
                yield return mediaDirOverride;
            string env = Environment.GetEnvironmentVariable("UAV_TEST_MEDIA_DIR");
            if (!string.IsNullOrEmpty(env) && Directory.Exists(env))
                yield return env;
            string probe = null;
            try { probe = Application.dataPath; } catch { probe = null; }
            for (int i = 0; i < 6 && !string.IsNullOrEmpty(probe); i++)
            {
                string cand = Path.Combine(probe, "tests", "media", "out");
                if (Directory.Exists(cand)) yield return cand;
                try { probe = Path.GetDirectoryName(probe); } catch { break; }
            }
        }
    }
}
