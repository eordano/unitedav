// SPDX-License-Identifier: Apache-2.0

using UnityEngine;
using UnitedAV;

namespace UnitedAV.Samples
{
    [RequireComponent(typeof(Renderer))]
    public class VideoQuadSample : MonoBehaviour
    {
        [Tooltip("Direct MP4/HLS URL or absolute file path.")]
        public string url = "https://example.com/video.mp4";

        private MediaPlayer _player;
        private Renderer _renderer;
        private Material _material;
        private bool _flipApplied;

        private void Awake()
        {
            _renderer = GetComponent<Renderer>();
            _material = _renderer.material;

            _player = gameObject.AddComponent<MediaPlayer>();
            _player.AutoOpen = false;
            _player.AudioVolume = 1f;
        }

        private void Start()
        {
            bool opened = _player.OpenMedia(MediaPathType.AbsolutePathOrURL, url, autoPlay: true);
            if (!opened)
                Debug.LogWarning($"[UnitedAV sample] OpenMedia failed for: {url}");

            _player.Events.AddListener(OnMediaEvent);
            Debug.Log($"[UnitedAV sample] HasListeners = {_player.Events.HasListeners()}");
        }

        private void Update()
        {
            ITextureProducer producer = _player.TextureProducer;
            if (producer == null)
                return;

            Texture tex = producer.GetTexture();
            if (tex == null)
                return;

            if (_material.mainTexture != tex)
                _material.mainTexture = tex;

            if (!_flipApplied && producer.RequiresVerticalFlip())
            {
                _material.mainTextureScale = new Vector2(1f, -1f);
                _material.mainTextureOffset = new Vector2(0f, 1f);
                _flipApplied = true;
            }
        }

        private void OnMediaEvent(MediaPlayer mp, MediaPlayerEvent.EventType et, ErrorCode code)
        {
            if (et == MediaPlayerEvent.EventType.Error)
                Debug.LogError($"[UnitedAV sample] media error: {code}");
            else
                Debug.Log($"[UnitedAV sample] event: {et}");
        }

        private void OnDestroy()
        {
            if (_player != null)
                _player.Events.RemoveAllListeners();
        }
    }
}
