#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# mediamtx launcher + publish helpers for the streaming matrix. Standalone
# (bash launch.sh [clip]) publishes a clip and waits for Ctrl-C; also sourced by
# run-matrix.sh for mtx_start/publish/stop and the url_* helpers. Ports must match
# tests/streaming/mediamtx.yml.
set -u

STREAM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$STREAM_DIR/../.." && pwd)"
MTX_CONF="$STREAM_DIR/mediamtx.yml"
RUN_DIR="${UAV_RUN_DIR:-$STREAM_DIR/run}"
mkdir -p "$RUN_DIR"

PATH_NAME="${UAV_MTX_PATH:-synthetic}"
RTSP_PORT=8554
RTMP_PORT=1936
HLS_PORT=8889
SRT_PORT=8890

url_rtsp() { echo "rtsp://127.0.0.1:${RTSP_PORT}/${PATH_NAME}"; }
url_rtmp() { echo "rtmp://127.0.0.1:${RTMP_PORT}/${PATH_NAME}"; }
url_hls()  { echo "http://127.0.0.1:${HLS_PORT}/${PATH_NAME}/index.m3u8"; }
url_srt()  { echo "srt://127.0.0.1:${SRT_PORT}?streamid=read:${PATH_NAME}"; }
url_srt_publish() { echo "srt://127.0.0.1:${SRT_PORT}?streamid=publish:${PATH_NAME}"; }

MTX_PID=""
PUB_PID=""

mtx_start() {
  mediamtx "$MTX_CONF" >"$RUN_DIR/mediamtx.log" 2>&1 &
  MTX_PID=$!
  for _ in $(seq 1 50); do
    if grep -q "\[RTSP\] listener opened" "$RUN_DIR/mediamtx.log" 2>/dev/null; then break; fi
    if ! kill -0 "$MTX_PID" 2>/dev/null; then
      echo "mediamtx failed to start:"; cat "$RUN_DIR/mediamtx.log"; return 1
    fi
    sleep 0.1
  done
  echo "mediamtx up (pid=$MTX_PID): RTSP :$RTSP_PORT RTMP :$RTMP_PORT HLS :$HLS_PORT SRT :$SRT_PORT"
}

# Publishes over RTSP, transcoding to H.264/AAC via VAAPI HW encode when a render
# node is present, else -c:v copy (valid only if the source is already H.264).
mtx_publish() {
  local clip="$1"
  local venc=(-c:v h264_vaapi)
  local vinit=(-vaapi_device "${UAV_VAAPI_DEV:-/dev/dri/renderD128}")
  local vfilt=(-vf 'format=nv12,hwupload')
  if [ ! -e "${UAV_VAAPI_DEV:-/dev/dri/renderD128}" ]; then
    vinit=(); vfilt=(); venc=(-c:v copy)
  fi
  ffmpeg -hide_banner -loglevel error "${vinit[@]}" -re -stream_loop -1 -i "$clip" \
    "${vfilt[@]}" "${venc[@]}" -c:a aac -b:a 128k \
    -f rtsp -rtsp_transport tcp "$(url_rtsp)" \
    >"$RUN_DIR/publish.log" 2>&1 &
  PUB_PID=$!
  for _ in $(seq 1 40); do
    if grep -q "is publishing to path" "$RUN_DIR/mediamtx.log" 2>/dev/null; then break; fi
    if ! kill -0 "$PUB_PID" 2>/dev/null; then
      echo "publisher exited early:"; cat "$RUN_DIR/publish.log"; return 1
    fi
    sleep 0.1
  done
  sleep 1
  echo "publishing $(basename "$clip") -> path '$PATH_NAME' (pid=$PUB_PID)"
}

mtx_publish_rtmp() {
  local clip="$1"
  local venc=(-c:v h264_vaapi)
  local vinit=(-vaapi_device "${UAV_VAAPI_DEV:-/dev/dri/renderD128}")
  local vfilt=(-vf 'format=nv12,hwupload')
  if [ ! -e "${UAV_VAAPI_DEV:-/dev/dri/renderD128}" ]; then
    vinit=(); vfilt=(); venc=(-c:v copy)
  fi
  ffmpeg -hide_banner -loglevel error "${vinit[@]}" -re -stream_loop -1 -i "$clip" \
    "${vfilt[@]}" "${venc[@]}" -c:a aac -b:a 128k \
    -f flv "$(url_rtmp)" \
    >"$RUN_DIR/publish_rtmp.log" 2>&1 &
  PUB_PID=$!
  for _ in $(seq 1 40); do
    if grep -q "is publishing to path" "$RUN_DIR/mediamtx.log" 2>/dev/null; then break; fi
    if ! kill -0 "$PUB_PID" 2>/dev/null; then
      echo "RTMP publisher exited early:"; cat "$RUN_DIR/publish_rtmp.log"; return 1
    fi
    sleep 0.1
  done
  sleep 1
  echo "publishing $(basename "$clip") via RTMP -> path '$PATH_NAME' (pid=$PUB_PID)"
}

mtx_serverside_check() {
  local url="$1" out
  out="$(timeout 15 ffprobe -hide_banner -loglevel error \
        -show_entries stream=codec_type,codec_name,width,height \
        -of default=nw=1 "$url" 2>/dev/null)"
  local w h vc
  w="$(echo "$out" | sed -nE 's/^width=([0-9]+)/\1/p' | head -1)"
  h="$(echo "$out" | sed -nE 's/^height=([0-9]+)/\1/p' | head -1)"
  vc="$(echo "$out" | sed -nE 's/^codec_name=(.+)/\1/p' | head -1)"
  if [ -n "$w" ] && [ -n "$h" ]; then echo "${w}x${h} ${vc}"; else echo "unreachable"; fi
}

mtx_stop() {
  [ -n "$PUB_PID" ] && kill "$PUB_PID" 2>/dev/null; wait "$PUB_PID" 2>/dev/null
  [ -n "$MTX_PID" ] && kill "$MTX_PID" 2>/dev/null; wait "$MTX_PID" 2>/dev/null
  PUB_PID=""; MTX_PID=""
}

if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
  CLIP="${1:-$REPO_DIR/tests/media/out/webm__vp9__opus.webm}"
  if [ ! -f "$CLIP" ]; then
    echo "clip not found: $CLIP  (run tests/media/gen.sh first)"; exit 1
  fi
  trap 'mtx_stop; exit 0' INT TERM
  mtx_start || exit 1
  mtx_publish "$CLIP" || { mtx_stop; exit 1; }
  echo
  echo "Pull URLs:"
  echo "  RTSP : $(url_rtsp)"
  echo "  RTMP : $(url_rtmp)"
  echo "  HLS  : $(url_hls)"
  echo "  SRT  : $(url_srt)"
  echo
  echo "Ctrl-C to stop."
  while kill -0 "$MTX_PID" 2>/dev/null; do sleep 1; done
fi
