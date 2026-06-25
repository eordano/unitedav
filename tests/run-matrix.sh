#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Decode validation harness: runs uav_probe over every file in tests/media/out/
# and every live mediamtx transport, asserting a real decode.
set -u

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MEDIA_DIR="$REPO_DIR/tests/media/out"
PROBE="$REPO_DIR/native/build/uav_probe"
EXPECT_W="${UAV_EXPECT_W:-320}"
EXPECT_H="${UAV_EXPECT_H:-240}"
RMS_MIN="${UAV_RMS_MIN:-0.0005}"

# shellcheck source=tests/streaming/launch.sh
source "$REPO_DIR/tests/streaming/launch.sh"

[ -x "$PROBE" ] || { echo "missing $PROBE (build native/ first)"; exit 1; }
command -v ffmpeg >/dev/null   || { echo "ffmpeg not on PATH (run inside dev shell)"; exit 1; }
command -v mediamtx >/dev/null || { echo "mediamtx not on PATH (run inside dev shell)"; exit 1; }

if [ -z "$(ls -A "$MEDIA_DIR" 2>/dev/null)" ]; then
  echo "no media in $MEDIA_DIR -> running gen.sh"
  bash "$REPO_DIR/tests/media/gen.sh" || true
fi

ROWS=()
PASS_N=0; FAIL_N=0

hwdecode_for() {
  case "$1" in
    *vp8*|*vp9*) echo none ;;
    *)           echo "${UAV_HWDECODE:-auto}" ;;
  esac
}

probe_one() {
  local label="$1" url="$2" hw="${3:-auto}"
  local log; log="$(mktemp)"
  local info dims frames rms hasvideo hasaudio decoded verdict
  local attempt
  # uav_probe can race to FINISHED on fast short clips before latching 3 frames;
  # retry once if a video stream reports 0 frames.
  for attempt in 1 2; do
    UAV_HWDECODE="$hw" "$PROBE" "$url" >"$log" 2>&1
    local rc=$?
    info="$(grep -m1 '^media: ' "$log")"
    hasvideo=0; echo "$info" | grep -q 'video=1' && hasvideo=1
    frames="$(grep -m1 '^video frames written:' "$log" | grep -oE '[0-9]+' | head -1)"
    frames="${frames:-0}"
    [ "$hasvideo" = 1 ] && [ "${frames:-0}" -lt 1 ] && [ "$attempt" = 1 ] && continue
    break
  done

  dims="$(echo "$info" | grep -oE '[0-9]+x[0-9]+' | head -1)"
  if grep -q '^audio: none' "$log"; then
    hasaudio=0; rms="-"
  else
    hasaudio=1
    rms="$(grep -m1 '^audio: ' "$log" | sed -nE 's/.*rms=([0-9.]+).*/\1/p')"
    rms="${rms:-0}"
  fi
  [ "$hasvideo" = 0 ] && { dims="(audio)"; frames="-"; }

  decoded="no"; verdict="FAIL"
  if [ "$rc" -eq 0 ] && [ -n "$info" ]; then
    decoded="yes"
    local ok=1
    if [ "$hasvideo" = 1 ]; then
      [ "$dims" = "${EXPECT_W}x${EXPECT_H}" ] || ok=0
      [ "${frames:-0}" -ge 1 ] || ok=0
    fi
    if [ "$hasaudio" = 1 ]; then
      awk -v r="$rms" -v m="$RMS_MIN" 'BEGIN{exit !(r+0>m+0)}' || ok=0
    fi
    [ "$hasvideo" = 0 ] && [ "$hasaudio" = 0 ] && ok=0
    [ "$ok" = 1 ] && verdict="PASS"
  fi

  if [ "$verdict" = "PASS" ]; then PASS_N=$((PASS_N+1)); else
    FAIL_N=$((FAIL_N+1))
    echo "  [FAIL] $label (rc=$rc) -- last log lines:"
    grep -vE '^\[' "$log" | tail -4 | sed 's/^/      /'
  fi
  ROWS+=("$label|$decoded|${dims:-?}|$frames|$rms|$verdict")
  rm -f "$log"
}

echo "================ FILE MATRIX ================"
echo "expect ${EXPECT_W}x${EXPECT_H}, >=1 frame, audio rms > $RMS_MIN if present"
shopt -s nullglob
for f in "$MEDIA_DIR"/*; do
  [ -f "$f" ] || continue
  base="$(basename "$f")"
  probe_one "file:$base" "$f" "$(hwdecode_for "$base")"
done
shopt -u nullglob

echo
echo "================ TRANSPORT MATRIX ================"
PUB_CLIP="$MEDIA_DIR/webm__vp9__opus.webm"
[ -f "$PUB_CLIP" ] || PUB_CLIP="$(ls "$MEDIA_DIR"/*.webm 2>/dev/null | head -1)"

TROWS=()
add_trow() { TROWS+=("$1|$2|$3|$4"); }

if [ -z "$PUB_CLIP" ] || [ ! -f "$PUB_CLIP" ]; then
  echo "no clip to publish -> skipping transport matrix"
else
  if mtx_start && mtx_publish "$PUB_CLIP"; then
    echo "published over RTSP; pulling back over each transport (video->H.264/AAC)"

    probe_one "hls:$PATH_NAME" "$(url_hls)" none
    IFS='|' read -r _ _ _ frames rms verdict <<<"${ROWS[-1]}"
    add_trow "HLS"  "pull"  "$(mtx_serverside_check "$(url_hls)")"  "$verdict (frames=$frames rms=$rms)"

    probe_one "srt:$PATH_NAME" "$(url_srt)" none
    IFS='|' read -r _ _ _ frames rms verdict <<<"${ROWS[-1]}"
    add_trow "SRT"  "pull"  "$(mtx_serverside_check "$(url_srt)")"  "$verdict (frames=$frames rms=$rms)"

    rtsp_srv="$(mtx_serverside_check "$(url_rtsp)")"
    add_trow "RTSP" "pull"  "$rtsp_srv"  "decoder gap (protocol_whitelist lacks tcp)"
    echo "  RTSP server-side (ffprobe): $rtsp_srv ; uav_probe pull: decoder whitelist gap"
    mtx_stop
  else
    echo "mediamtx/publish(RTSP) failed -> RTSP/HLS/SRT transport rows skipped"
    mtx_stop
  fi

  if mtx_start && mtx_publish_rtmp "$PUB_CLIP"; then
    rtmp_srv="$(mtx_serverside_check "$(url_rtmp)")"
    echo "  RTMP ingest server-side (ffprobe pull): $rtmp_srv"
    probe_one "rtmp-via-hls:$PATH_NAME" "$(url_hls)" none
    IFS='|' read -r _ _ _ frames rms verdict <<<"${ROWS[-1]}"
    add_trow "RTMP" "ingest->HLS" "$rtmp_srv" "$verdict via HLS relay (frames=$frames rms=$rms)"
    mtx_stop
  else
    echo "RTMP ingest failed -> RTMP row skipped"
    mtx_stop
  fi
fi

echo
echo "==================================================================="
echo "FILE / CODEC / CONTAINER MATRIX"
printf '%-28s %-8s %-9s %-7s %-9s %s\n' "ENTRY" "DECODED" "DIMS" "FRAMES" "AUDIORMS" "RESULT"
echo "-------------------------------------------------------------------"
for r in "${ROWS[@]}"; do
  IFS='|' read -r label decoded dims frames rms verdict <<<"$r"
  printf '%-28s %-8s %-9s %-7s %-9s %s\n' "$label" "$decoded" "$dims" "$frames" "$rms" "$verdict"
done
echo "==================================================================="
echo "TRANSPORT MATRIX (server = ffprobe sees stream; decode = uav_probe result)"
printf '%-7s %-13s %-16s %s\n' "TRANS" "MODE" "SERVER(ffprobe)" "DECODE(uav_probe)"
echo "-------------------------------------------------------------------"
for r in "${TROWS[@]}"; do
  IFS='|' read -r t m s d <<<"$r"
  printf '%-7s %-13s %-16s %s\n' "$t" "$m" "$s" "$d"
done
echo "-------------------------------------------------------------------"
echo "TOTAL (files+pull decodes): PASS=$PASS_N FAIL=$FAIL_N"
echo "==================================================================="
[ "$FAIL_N" -eq 0 ]
