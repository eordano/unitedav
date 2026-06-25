#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Verify the mic + speaker test page (tests/web/av_io_test.html) headlessly:
# fake audio devices (--use-fake-device-for-media-stream emits a tone, no TCC) +
# auto-granted permission. The page console.log's "UAVRESULT PASS/FAIL ...".
#
# Browser: prefers Google Chrome, falls back to Chromium (same as run-webgpu.sh).
set -u
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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

PORT="${UAV_WEB_PORT:-8139}"
RUN="$SELF_DIR/run-avio"; rm -rf "$RUN"; mkdir -p "$RUN/profile"
cp "$SELF_DIR/av_io_test.html" "$RUN/index.html"
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$RUN" >/dev/null 2>&1 &
SRV=$!
trap 'kill "$SRV" 2>/dev/null; rm -rf "$RUN"' EXIT
sleep 0.6

OUT="$RUN/chrome.log"
echo "[avio] browser: $CHROME"
timeout 40 "$CHROME" \
  --headless=new --no-sandbox \
  --use-fake-device-for-media-stream --use-fake-ui-for-media-stream \
  --autoplay-policy=no-user-gesture-required \
  --user-data-dir="$RUN/profile" --enable-logging=stderr --v=1 \
  "http://127.0.0.1:$PORT/index.html?auto" >"$OUT" 2>&1 &
CH=$!
RESULT=""
for _ in $(seq 1 30); do
  RESULT="$(grep -aoE 'UAVRESULT .*' "$OUT" 2>/dev/null | sed 's/", source.*//' | head -1)"
  [ -n "$RESULT" ] && break
  sleep 1
done
kill "$CH" 2>/dev/null

[ -n "$RESULT" ] || { echo "AVIO-FAIL: no UAVRESULT. Log tail:"; tail -12 "$OUT" | sed 's/^/  /'; exit 1; }
echo "[avio] $RESULT"
case "$RESULT" in
  *"UAVRESULT PASS"*) echo "AVIO-PASS: mic + speaker verified"; exit 0 ;;
  *) echo "AVIO-FAIL: $RESULT"; exit 1 ;;
esac
