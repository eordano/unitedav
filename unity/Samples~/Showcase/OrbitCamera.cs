// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace UnitedAV.Samples.Showcase
{
    [DisallowMultipleComponent]
    public sealed class OrbitCamera : MonoBehaviour
    {
        public Vector3 target = Vector3.zero;
        public float radius = 11f;
        public float height = 3.5f;
        [Tooltip("Degrees per second around the ring.")]
        public float degreesPerSecond = 18f;
        [Tooltip("Amplitude of the gentle height bob (world units).")]
        public float bobAmplitude = 1.1f;
        [Tooltip("Amplitude of the gentle dolly in/out (world units).")]
        public float dollyAmplitude = 1.6f;

        private float _angleDeg;

        private void Update()
        {
            _angleDeg += degreesPerSecond * Time.deltaTime;
            float t = Time.time;

            float r = radius + Mathf.Sin(t * 0.23f) * dollyAmplitude;
            float h = height + Mathf.Sin(t * 0.37f) * bobAmplitude;

            float rad = _angleDeg * Mathf.Deg2Rad;
            var pos = new Vector3(Mathf.Sin(rad) * r, h, Mathf.Cos(rad) * r);
            transform.position = pos;
            transform.rotation = Quaternion.LookRotation((target - pos).normalized, Vector3.up);
        }
    }
}
