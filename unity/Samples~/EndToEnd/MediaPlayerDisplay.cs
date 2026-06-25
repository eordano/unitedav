// SPDX-License-Identifier: Apache-2.0

using UnityEngine;
using UnityEngine.UI;
using UnitedAV;

namespace UnitedAV.Samples.EndToEnd
{
    public sealed class MediaPlayerDisplay : MonoBehaviour
    {
        [Tooltip("Player to display. If null, found on this GameObject or in the scene.")]
        public MediaPlayer player;

        [Tooltip("Optional UI target. Texture is assigned to RawImage.texture.")]
        public RawImage rawImage;

        [Tooltip("Optional 3D target. Texture is assigned to renderer.material.mainTexture.")]
        public Renderer targetRenderer;

        private Material _materialInstance;
        private Texture _boundTexture;
        private bool _flipResolved;

        private void Awake()
        {
            if (player == null)
                player = GetComponent<MediaPlayer>();
            if (player == null)
                player = FindAnyMediaPlayer();

            if (targetRenderer != null)
                _materialInstance = targetRenderer.material;
        }

        private void Update()
        {
            if (player == null)
                return;

            ITextureProducer producer = player.TextureProducer;
            if (producer == null)
                return;

            Texture tex = producer.GetTexture();
            if (tex == null)
                return;

            bool textureChanged = tex != _boundTexture;
            if (textureChanged)
            {
                _boundTexture = tex;
                _flipResolved = false;

                if (rawImage != null)
                    rawImage.texture = tex;
                if (_materialInstance != null)
                    _materialInstance.mainTexture = tex;
            }

            if (!_flipResolved)
            {
                ApplyFlip(producer.RequiresVerticalFlip());
                _flipResolved = true;
            }
        }

        private void ApplyFlip(bool flipV)
        {
            if (rawImage != null)
                rawImage.uvRect = flipV ? new Rect(0f, 1f, 1f, -1f)
                                        : new Rect(0f, 0f, 1f, 1f);

            if (_materialInstance != null)
            {
                _materialInstance.mainTextureScale = flipV ? new Vector2(1f, -1f) : Vector2.one;
                _materialInstance.mainTextureOffset = flipV ? new Vector2(0f, 1f) : Vector2.zero;
            }
        }

        private static MediaPlayer FindAnyMediaPlayer()
        {
#if UNITY_2023_1_OR_NEWER
            return Object.FindFirstObjectByType<MediaPlayer>();
#else
            return Object.FindObjectOfType<MediaPlayer>();
#endif
        }
    }
}
