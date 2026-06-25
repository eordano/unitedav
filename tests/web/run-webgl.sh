#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Verify the WebGL backend's core: browser-decode a clip into a <video>, upload to
# a WebGL2 texture, read it back, and check it matches the same frame (2D canvas) +
# an ffmpeg oracle. Runs headless Chromium; the page console.log's "UAVRESULT ...".
#
# Uses a VP9 webm clip (open codec; Chromium always decodes it — its H.264 support
# is build-dependent).
set -u
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SELF_DIR/../.." && pwd)"
MEDIA="$REPO_DIR/tests/media/out"

CLIP_SRC="${UAV_WEB_CLIP:-$MEDIA/webm__vp9__opus.webm}"
[ -f "$CLIP_SRC" ] || CLIP_SRC="$MEDIA/webm__vp8__vorbis.webm"
if [ ! -f "$CLIP_SRC" ]; then
  echo "WEB-SKIP: no webm clip in $MEDIA (run tests/media/gen.sh)"; exit 0
fi

# Prefer Google Chrome (the real target); fall back to Chromium only where Chrome
# isn't installed (e.g. this Linux box). Honor UAV_WEB_BROWSER if set.
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
# ffmpeg for the oracle (optional — the gate is webgl_vs_2d, oracle is a bonus).
FFMPEG="${UAV_FFMPEG:-}"
if [ -z "$FFMPEG" ]; then
  if command -v ffmpeg >/dev/null 2>&1; then FFMPEG="ffmpeg"; fi
fi

RUN="$SELF_DIR/run"; rm -rf "$RUN"; mkdir -p "$RUN"
cp "$SELF_DIR/webgl_probe.html" "$RUN/index.html"
cp "$CLIP_SRC" "$RUN/clip.webm"
# ffmpeg oracle: first frame, RGBA, normalized to 320x240 (the harness canvas).
if [ -n "$FFMPEG" ]; then
  # shellcheck disable=SC2086
  $FFMPEG -hide_banner -loglevel error -i "$RUN/clip.webm" -vf scale=320:240 -frames:v 1 \
    -pix_fmt rgba -f rawvideo "$RUN/oracle.rgba" 2>/dev/null || true
fi
[ -s "$RUN/oracle.rgba" ] || echo "[web] note: no oracle.rgba (oracle check skipped)"

PORT="${UAV_WEB_PORT:-8137}"
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$RUN" >/dev/null 2>&1 &
SRV=$!
trap 'kill "$SRV" 2>/dev/null' EXIT
sleep 0.6

PROFILE="$RUN/profile"; mkdir -p "$PROFILE"
OUT="$RUN/chrome.log"
echo "[web] browser: $CHROME"
echo "[web] clip: $CLIP_SRC"
timeout 40 "$CHROME" \
  --headless=new --no-sandbox --disable-gpu \
  --enable-unsafe-swiftshader --use-angle=swiftshader \
  --autoplay-policy=no-user-gesture-required \
  --user-data-dir="$PROFILE" \
  --enable-logging=stderr --v=1 \
  "http://127.0.0.1:$PORT/index.html" >"$OUT" 2>&1 &
CH=$!
# Poll the log for the result line (don't depend on exact chromium lifetime).
RESULT=""
for _ in $(seq 1 40); do
  RESULT="$(grep -aoE 'UAVRESULT .*' "$OUT" 2>/dev/null | head -1)"
  [ -n "$RESULT" ] && break
  sleep 1
done
kill "$CH" 2>/dev/null

if [ -z "$RESULT" ]; then
  echo "WEB-FAIL: no UAVRESULT from the page. Last chromium log lines:"
  tail -15 "$OUT" 2>/dev/null | sed 's/^/  /'
  exit 1
fi
echo "[web] $RESULT"
case "$RESULT" in
  *"UAVRESULT PASS"*) echo "WEB-PASS: WebGL video->texture verified"; exit 0 ;;
  *) echo "WEB-FAIL: $RESULT"; exit 1 ;;
esac
