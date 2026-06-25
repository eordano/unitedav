#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Verify the WebGPU backend's core: browser-decode a clip into a <video>, upload to
# a GPUTexture (copyExternalImageToTexture), render+read it back, and check it
# matches the same frame (2D canvas) + an ffmpeg oracle. Runs a headless browser
# with WebGPU enabled; the page console.log's "UAVRESULT ...".
#
# Uses a VP9 webm clip (open codec; browsers always decode it).
#
# Browser: prefers Google Chrome (auto-detected, incl. /Applications/Google Chrome.app
# on macOS), falling back to Chromium only where Chrome isn't installed.
# Hosts:
#   SHADE (mac, canonical): Google Chrome auto-detected; WebGPU is Metal-3 backed.
#     UAV_FFMPEG="nix run nixpkgs#ffmpeg --" bash tests/web/run-webgpu.sh
#     (homebrew chromium on SHADE is Killed:9 — Chrome is required there.)
#   Linux (fallback): no Chrome here -> Chromium, with --enable-unsafe-webgpu
#     --enable-features=Vulkan (Dawn/Vulkan; needs --use-angle=vulkan for a real adapter).
#
# Env overrides:
#   UAV_WEB_BROWSER  path to a Chrome/Chromium binary (else Chrome-first autodetect)
#   UAV_FFMPEG       ffmpeg command (e.g. "nix run nixpkgs#ffmpeg --"); else `ffmpeg`
#   UAV_WEB_CLIP     path to the source webm clip
#   UAV_WEB_PORT     http.server port (default 8138)
#   UAV_WEB_MODE     'webcodecs' (default, correct color) | 'copy' | 'external'
#   UAV_WEB_FLAGS    extra browser flags (appended)
set -u
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SELF_DIR/../.." && pwd)"
MEDIA="$REPO_DIR/tests/media/out"

CLIP_SRC="${UAV_WEB_CLIP:-$MEDIA/webm__vp9__opus.webm}"
[ -f "$CLIP_SRC" ] || CLIP_SRC="$MEDIA/webm__vp8__vorbis.webm"
if [ ! -f "$CLIP_SRC" ]; then
  echo "WEB-SKIP: no webm clip in $MEDIA (run tests/media/gen.sh)"; exit 0
fi

# Prefer Google Chrome (the real WebGPU/WebCodecs target); fall back to Chromium
# only where Chrome isn't installed (e.g. this Linux box).
CHROME="${UAV_WEB_BROWSER:-}"
if [ -z "$CHROME" ]; then
  for cand in \
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
    "/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary"; do
    [ -x "$cand" ] && { CHROME="$cand"; break; }
  done
  [ -n "$CHROME" ] || CHROME="$(command -v google-chrome || command -v google-chrome-stable \
    || command -v chrome || command -v chromium || command -v chromium-browser || true)"
fi
[ -n "$CHROME" ] || { echo "WEB-SKIP: no Chrome/Chromium (set UAV_WEB_BROWSER)"; exit 0; }

# ffmpeg for the oracle (optional — the gate is webgpu_vs_2d, oracle is a bonus).
FFMPEG="${UAV_FFMPEG:-}"
if [ -z "$FFMPEG" ]; then
  if command -v ffmpeg >/dev/null 2>&1; then FFMPEG="ffmpeg"; fi
fi

MODE="${UAV_WEB_MODE:-webcodecs}"

RUN="$SELF_DIR/run-gpu"; rm -rf "$RUN"; mkdir -p "$RUN"
cp "$SELF_DIR/webgpu_probe.html" "$RUN/index.html"
cp "$CLIP_SRC" "$RUN/clip.webm"

# ffmpeg oracle: first frame, RGBA, normalized to 320x240 (the harness canvas).
if [ -n "$FFMPEG" ]; then
  # shellcheck disable=SC2086
  $FFMPEG -hide_banner -loglevel error -i "$RUN/clip.webm" -vf scale=320:240 -frames:v 1 \
    -pix_fmt rgba -f rawvideo "$RUN/oracle.rgba" 2>/dev/null || true
fi
[ -s "$RUN/oracle.rgba" ] || echo "[web] note: no oracle.rgba (oracle check will be skipped)"

PORT="${UAV_WEB_PORT:-8138}"
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$RUN" >/dev/null 2>&1 &
SRV=$!
trap 'kill "$SRV" 2>/dev/null' EXIT
sleep 0.6

PROFILE="$RUN/profile"; mkdir -p "$PROFILE"
OUT="$RUN/browser.log"

# Per-host WebGPU flags. macOS Chrome uses ANGLE/Metal; Linux uses Dawn/Vulkan.
# NOTE: on a headless Linux box, the SwiftShader/--disable-gpu WebGPU adapter dies at
# mapAsync ("valid external Instance reference no longer exists"); a real adapter is
# only kept alive with --use-angle=vulkan (verified with intel/gen-12lp).
case "$(uname -s)" in
  Darwin) HOST_FLAGS="--use-angle=metal" ;;
  *)      HOST_FLAGS="--use-angle=vulkan --enable-features=Vulkan" ;;
esac

echo "[web] browser: $CHROME"
echo "[web] clip:    $CLIP_SRC"
echo "[web] mode:    $MODE"
# shellcheck disable=SC2086
timeout 60 "$CHROME" \
  --headless=new --no-sandbox \
  --enable-unsafe-webgpu $HOST_FLAGS \
  --disable-accelerated-video-decode \
  --autoplay-policy=no-user-gesture-required \
  --user-data-dir="$PROFILE" \
  --enable-logging=stderr --v=1 \
  ${UAV_WEB_FLAGS:-} \
  "http://127.0.0.1:$PORT/index.html?mode=$MODE" >"$OUT" 2>&1 &
CH=$!
# Poll the log for the result line (don't depend on exact browser lifetime).
RESULT=""
for _ in $(seq 1 50); do
  RESULT="$(grep -aoE 'UAVRESULT [^,]*' "$OUT" 2>/dev/null | head -1)"
  [ -n "$RESULT" ] && break
  sleep 1
done
kill "$CH" 2>/dev/null

if [ -z "$RESULT" ]; then
  echo "WEB-FAIL: no UAVRESULT from the page. Last browser log lines:"
  tail -20 "$OUT" 2>/dev/null | sed 's/^/  /'
  exit 1
fi
echo "[web] $RESULT"
case "$RESULT" in
  *"UAVRESULT PASS"*) echo "WEB-PASS: WebGPU video->texture verified"; exit 0 ;;
  *"no-webgpu"*|*"no-adapter"*) echo "WEB-SKIP: WebGPU unavailable in this browser/host"; exit 0 ;;
  *) echo "WEB-FAIL: $RESULT"; exit 1 ;;
esac
