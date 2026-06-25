#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Record a demo of UnitedAV on macOS: native Showcase player, then the WebGL web
# player, then the WebGPU web player — into one .mov.
#
# RUN THIS IN SHADE'S OWN Terminal (not over SSH): macOS Screen Recording is gated
# by TCC, which an SSH session can't satisfy. The first run pops a permission
# dialog — grant "Screen Recording" to Terminal, then re-run.
set -u
REPO="${UAV_REPO:-/Users/usr/unitedav}"
MEDIA="$REPO/tests/media/out"
APP="${UAV_SHOWCASE_APP:-$REPO/tests/unity/Build/showcase-osx/Showcase.app}"
WEB="$REPO/tests/web"
OUT="${UAV_DEMO_OUT:-$HOME/uav-demo.mov}"
PORT="${UAV_DEMO_PORT:-8177}"
CHROME="${UAV_WEB_BROWSER:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
SEG="${UAV_DEMO_SEG:-11}"        # seconds per segment
CLIP="${UAV_DEMO_CLIP:-$MEDIA/webm__vp9__opus.webm}"

command -v screencapture >/dev/null || { echo "no screencapture"; exit 1; }
[ -f "$CLIP" ] || { echo "no clip at $CLIP (run tests/media/gen.sh)"; exit 1; }

# Serve the web pages + clip same-origin (http: file:// can't do video/WebGPU/mic).
SERVE="$(mktemp -d)"; PROFILE="$SERVE/chrome"
cp "$WEB/web_player.html" "$SERVE/index.html"; cp "$CLIP" "$SERVE/clip.webm"
cp "$WEB/av_io_test.html" "$SERVE/avio.html"
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$SERVE" >/dev/null 2>&1 &
SRV=$!
cleanup(){ kill "$SRV" 2>/dev/null; pkill -f 'Showcase.app/Contents/MacOS' 2>/dev/null;
  pkill -f "user-data-dir=$PROFILE" 2>/dev/null; rm -rf "$SERVE"; }
trap cleanup EXIT
sleep 1

# $1=url, $2..=extra flags (e.g. fake audio devices for the mic/speaker segment).
chrome_win(){ local url="$1"; shift; "$CHROME" --user-data-dir="$PROFILE" --no-first-run \
  --no-default-browser-check --new-window --window-size=1280,800 --window-position=120,80 \
  --autoplay-policy=no-user-gesture-required "$@" --app="$url" >/dev/null 2>&1 & }

TOTAL=$(( SEG*4 + 5 ))
echo "[demo] recording ${TOTAL}s -> $OUT"
echo "[demo] (if macOS denies the first capture, grant Terminal Screen Recording and re-run)"
screencapture -V "$TOTAL" "$OUT" &
CAP=$!
sleep 2

# 1) Native Showcase ---------------------------------------------------------
if [ -d "$APP" ]; then
  echo "[demo] 1/4 native Showcase"
  UAV_TEST_MEDIA_DIR="$MEDIA" open -F "$APP"
  sleep "$SEG"
  pkill -f 'Showcase.app/Contents/MacOS' 2>/dev/null; sleep 1
else
  echo "[demo] native Showcase.app missing ($APP) — skipping; build it with tests/unity/build-showcase-on-mac.sh"
fi

# 2) WebGL -------------------------------------------------------------------
echo "[demo] 2/4 WebGL"
chrome_win "http://127.0.0.1:$PORT/index.html?mode=webgl"
sleep "$SEG"
pkill -f "user-data-dir=$PROFILE" 2>/dev/null; sleep 1

# 3) WebGPU ------------------------------------------------------------------
# --disable-accelerated-video-decode forces software decode so the WebCodecs path
# gets raw YUV (HW frames hide it as format=null) -> exact colour. 320x240 SW is cheap.
echo "[demo] 3/4 WebGPU"
chrome_win "http://127.0.0.1:$PORT/index.html?mode=webgpu" --disable-accelerated-video-decode
sleep "$SEG"
pkill -f "user-data-dir=$PROFILE" 2>/dev/null; sleep 1

# 4) Mic + Speaker -----------------------------------------------------------
# Fake audio devices so the meters move unattended (no mic-permission prompt). For a
# REAL mic/speaker demo, drop these two flags and approve the prompt.
echo "[demo] 4/4 Mic + Speaker"
chrome_win "http://127.0.0.1:$PORT/avio.html?auto" \
  --use-fake-device-for-media-stream --use-fake-ui-for-media-stream
sleep "$SEG"

wait "$CAP" 2>/dev/null
echo "[demo] done -> $OUT"
ls -la "$OUT" 2>/dev/null
