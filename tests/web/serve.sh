#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Serve the web test/demo pages + a clip over http on 127.0.0.1, so you can open
# them in a browser. WebCodecs / WebGPU / getUserMedia need a real http origin and
# a secure context — file:// is a unique origin (CORS-blocked) and not secure, so
# the pages WON'T work opened as files. 127.0.0.1 counts as a secure context.
#
#   bash tests/web/serve.sh            # serve + print URLs
#   UAV_OPEN=1 bash tests/web/serve.sh # also open the WebGPU player in a browser
set -u
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SELF_DIR/../.." && pwd)"
MEDIA="$REPO_DIR/tests/media/out"
PORT="${UAV_WEB_PORT:-8137}"
CLIP="${UAV_WEB_CLIP:-$MEDIA/webm__vp9__opus.webm}"
[ -f "$CLIP" ] || CLIP="$MEDIA/webm__vp8__vorbis.webm"

RUN="$SELF_DIR/serve-root"; rm -rf "$RUN"; mkdir -p "$RUN"
for f in web_player.html webgpu_probe.html webgl_probe.html av_io_test.html webgpu_showcase.html; do
  [ -f "$SELF_DIR/$f" ] && cp "$SELF_DIR/$f" "$RUN/"
done
[ -f "$CLIP" ] && cp "$CLIP" "$RUN/clip.webm"
# A few distinct clips for the multi-screen showcase (clip0/1/2; falls back to clip).
i=0; for c in webm__vp9__opus.webm webm__vp8__vorbis.webm webm__av1__opus.webm; do
  [ -f "$MEDIA/$c" ] && cp "$MEDIA/$c" "$RUN/clip$i.webm"; i=$((i+1))
done

B="http://127.0.0.1:$PORT"
echo "Serving $RUN on $B/   (Ctrl-C to stop)"
echo "  WebGL  player : $B/web_player.html?mode=webgl"
echo "  WebGPU player : $B/web_player.html?mode=webgpu"
echo "  WebGPU showcase (multi-screen) : $B/webgpu_showcase.html"
echo "  Mic + Speaker : $B/av_io_test.html"
echo "Note: the WebGPU exact-colour path needs raw YUV (software decode). Open Chrome"
echo "      with --disable-accelerated-video-decode for exact colour; otherwise WebGPU"
echo "      still plays via the browser's own conversion. (UAV_OPEN=1 adds the flag.)"
if [ -n "${UAV_OPEN:-}" ]; then
  U="$B/web_player.html?mode=webgpu"
  CH="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
  FLAGS="--new-window --disable-accelerated-video-decode --autoplay-policy=no-user-gesture-required"
  if [ -x "$CH" ]; then "$CH" $FLAGS "$U" >/dev/null 2>&1 &
  elif command -v google-chrome >/dev/null 2>&1; then google-chrome $FLAGS "$U" >/dev/null 2>&1 &
  elif command -v chromium >/dev/null 2>&1; then chromium $FLAGS "$U" >/dev/null 2>&1 &
  elif command -v open >/dev/null 2>&1; then open "$U"
  elif command -v xdg-open >/dev/null 2>&1; then xdg-open "$U"; fi
fi
exec python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$RUN"
