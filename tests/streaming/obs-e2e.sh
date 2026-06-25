#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# End-to-end broadcast test: OBS Studio -> mediamtx -> our decoder. Headless OBS
# broadcasts a looping 320x240 H.264/AAC clip via RTMP -> mediamtx, then we pull
# it back over HLS and RTSP and decode with uav_probe, asserting real decode.
# Run inside the dev shell (obs/xvfb-run/obs-cmd/mediamtx/ffmpeg/uav_probe on PATH):
#   nix --extra-experimental-features 'nix-command flakes' develop \
#     'path:/<repo>' -c bash tests/streaming/obs-e2e.sh
#
# Set OBS_USE_WEBSOCKET=1 to start the stream via obs-cmd instead of --startstreaming.
set -u

STREAM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$STREAM_DIR/../.." && pwd)"
OBS_DIR="$STREAM_DIR/obs"
MTX_CONF="$STREAM_DIR/mediamtx.yml"
PROBE="$REPO_DIR/native/build/uav_probe"

# Live OBS path = <service.server tail>/<service.key> = live/test.
MTX_PATH="live/test"
RTSP_PORT=8554; RTMP_PORT=1936; HLS_PORT=8889
HLS_URL="http://127.0.0.1:${HLS_PORT}/${MTX_PATH}/index.m3u8"
RTSP_URL="rtsp://127.0.0.1:${RTSP_PORT}/${MTX_PATH}"

PROFILE="UnitedAV"
COLLECTION="UnitedAV"
CLIP="$OBS_DIR/clip.mp4"

EXPECT_W="${UAV_EXPECT_W:-320}"
EXPECT_H="${UAV_EXPECT_H:-240}"
RMS_MIN="${UAV_RMS_MIN:-0.0005}"
PUB_TIMEOUT="${UAV_OBS_PUB_TIMEOUT:-40}"
OBS_WS_PORT="${OBS_WS_PORT:-4455}"

# Private XDG_CONFIG_HOME/HOME keeps OBS entirely off the real ~/.config.
RUN_DIR="${UAV_OBS_RUN_DIR:-$STREAM_DIR/run-obs}"

MTX_PID=""; OBS_PID=""

log() { echo "[obs-e2e] $*"; }

mtx_start() {
  mediamtx "$MTX_CONF" >"$RUN_DIR/mediamtx.log" 2>&1 &
  MTX_PID=$!
  for _ in $(seq 1 50); do
    grep -q "\[RTSP\] listener opened" "$RUN_DIR/mediamtx.log" 2>/dev/null && break
    if ! kill -0 "$MTX_PID" 2>/dev/null; then
      log "mediamtx failed to start:"; cat "$RUN_DIR/mediamtx.log"; return 1
    fi
    sleep 0.1
  done
  log "mediamtx up (pid=$MTX_PID): RTMP :$RTMP_PORT RTSP :$RTSP_PORT HLS :$HLS_PORT"
}

obs_seed_config() {
  rm -rf "$RUN_DIR/config" "$RUN_DIR/.config"
  mkdir -p "$RUN_DIR/config"
  cp -r "$OBS_DIR/config-template/obs-studio" "$RUN_DIR/config/obs-studio"
  sed -i "s|@CLIP@|$CLIP|g" "$RUN_DIR/config/obs-studio/basic/scenes/${COLLECTION}.json"
}

obs_start() {
  [ -f "$CLIP" ] || { log "missing clip $CLIP (regenerate; see obs/README)"; return 1; }
  obs_seed_config

  (
    export QT_QPA_PLATFORM=xcb
    export LIBGL_ALWAYS_SOFTWARE=1
    export XDG_CONFIG_HOME="$RUN_DIR/config"
    export HOME="$RUN_DIR"
    local startflag=(--startstreaming)
    [ "${OBS_USE_WEBSOCKET:-0}" = 1 ] && startflag=()
    exec xvfb-run -a -s "-screen 0 1280x720x24" \
      obs --multi --minimize-to-tray "${startflag[@]}" \
      --profile "$PROFILE" --collection "$COLLECTION" \
      >"$RUN_DIR/obs.log" 2>&1
  ) &
  OBS_PID=$!
  log "OBS launching (pid=$OBS_PID) -> rtmp://127.0.0.1:${RTMP_PORT}/${MTX_PATH%/*}"

  if [ "${OBS_USE_WEBSOCKET:-0}" = 1 ]; then
    for _ in $(seq 1 50); do
      grep -q "\[obs-websocket\].*Server started" "$RUN_DIR/obs.log" 2>/dev/null && break
      kill -0 "$OBS_PID" 2>/dev/null || break
      sleep 0.2
    done
    sleep 1
    log "starting stream via obs-cmd (websocket :$OBS_WS_PORT)"
    obs-cmd --websocket "obs://127.0.0.1:${OBS_WS_PORT}" streaming start \
      >>"$RUN_DIR/obs.log" 2>&1 || \
      log "obs-cmd start returned non-zero (may already be streaming)"
  fi

  local n=$((PUB_TIMEOUT * 2))
  for _ in $(seq 1 "$n"); do
    grep -q "is publishing to path '${MTX_PATH}'" "$RUN_DIR/mediamtx.log" 2>/dev/null && {
      log "OBS is publishing to '${MTX_PATH}'"; sleep 3; return 0;
    }
    if ! kill -0 "$OBS_PID" 2>/dev/null; then
      log "OBS exited before publishing:"; tail -15 "$RUN_DIR/obs.log"; return 1
    fi
    sleep 0.5
  done
  log "timed out waiting for OBS to publish (${PUB_TIMEOUT}s):"
  tail -15 "$RUN_DIR/obs.log"
  return 1
}

obs_stop() {
  [ -n "$OBS_PID" ] && kill "$OBS_PID" 2>/dev/null
  # OBS spawns under xvfb-run (Xvfb + a wrapper); reap the whole subtree.
  pkill -f "obs --multi" 2>/dev/null
  pkill -f "Xvfb" 2>/dev/null
  [ -n "$OBS_PID" ] && wait "$OBS_PID" 2>/dev/null
  OBS_PID=""
}

mtx_stop() {
  [ -n "$MTX_PID" ] && kill "$MTX_PID" 2>/dev/null && wait "$MTX_PID" 2>/dev/null
  MTX_PID=""
}

teardown() { obs_stop; mtx_stop; }

ffprobe_check() {
  local url="$1" extra=()
  [[ "$url" == rtsp://* ]] && extra=(-rtsp_transport tcp)
  local out
  out="$(timeout 20 ffprobe -hide_banner -loglevel error "${extra[@]}" \
        -show_entries stream=codec_type,codec_name,width,height \
        -of default=nw=1 "$url" 2>/dev/null)"
  local w h
  w="$(echo "$out" | sed -nE 's/^width=([0-9]+)/\1/p' | head -1)"
  h="$(echo "$out" | sed -nE 's/^height=([0-9]+)/\1/p' | head -1)"
  [ -n "$w" ] && [ -n "$h" ] && echo "${w}x${h}" || echo "unreachable"
}

probe_decode() {
  local url="$1" log; log="$(mktemp)"
  UAV_HWDECODE=none timeout 30 "$PROBE" "$url" >"$log" 2>&1
  local rc=$? info dims frames rms verdict ok=1
  info="$(grep -m1 '^media: ' "$log")"
  dims="$(echo "$info" | grep -oE '[0-9]+x[0-9]+' | head -1)"
  frames="$(grep -m1 '^video frames written:' "$log" | grep -oE '[0-9]+' | head -1)"
  frames="${frames:-0}"
  if grep -q '^audio: none' "$log"; then rms="-"; else
    rms="$(grep -m1 '^audio: ' "$log" | sed -nE 's/.*rms=([0-9.]+).*/\1/p')"; rms="${rms:-0}"
  fi
  [ "$rc" -eq 0 ] || ok=0
  [ -n "$info" ] || ok=0
  [ "$dims" = "${EXPECT_W}x${EXPECT_H}" ] || ok=0
  [ "${frames:-0}" -ge 1 ] || ok=0
  if [ "$rms" != "-" ]; then
    awk -v r="$rms" -v m="$RMS_MIN" 'BEGIN{exit !(r+0>m+0)}' || ok=0
  fi
  verdict=FAIL; [ "$ok" = 1 ] && verdict=PASS
  if [ "$verdict" = FAIL ]; then
    echo "  [FAIL] $url (rc=$rc) -- last log lines:" >&2
    grep -vE '^\[' "$log" | tail -5 | sed 's/^/      /' >&2
  fi
  rm -f "$log"
  echo "${verdict}|${dims:-?}|${frames}|${rms}"
}

if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
  [ -x "$PROBE" ] || { echo "missing $PROBE (build native/ first)"; exit 1; }
  for t in obs xvfb-run mediamtx ffmpeg ffprobe; do
    command -v "$t" >/dev/null || { echo "$t not on PATH (run inside dev shell)"; exit 1; }
  done
  mkdir -p "$RUN_DIR"
  trap 'teardown; exit 130' INT TERM

  mtx_start || { teardown; exit 1; }
  if ! obs_start; then teardown; exit 1; fi

  echo
  echo "OBS broadcast running. Pulling back with uav_probe (SW decode):"
  HLS_SRV="$(ffprobe_check "$HLS_URL")"
  RTSP_SRV="$(ffprobe_check "$RTSP_URL")"
  HLS_RES="$(probe_decode "$HLS_URL")"
  RTSP_RES="$(probe_decode "$RTSP_URL")"

  teardown

  IFS='|' read -r hv hd hf hr <<<"$HLS_RES"
  IFS='|' read -r rv rd rf rr <<<"$RTSP_RES"
  echo
  echo "==================================================================="
  echo "OBS -> mediamtx -> uav_probe  END-TO-END BROADCAST"
  printf '%-7s %-16s %-7s %-8s %-9s %s\n' "TRANS" "SERVER(ffprobe)" "DIMS" "FRAMES" "AUDIORMS" "DECODE"
  echo "-------------------------------------------------------------------"
  printf '%-7s %-16s %-7s %-8s %-9s %s\n' "HLS"  "$HLS_SRV"  "$hd" "$hf" "$hr" "$hv"
  printf '%-7s %-16s %-7s %-8s %-9s %s\n' "RTSP" "$RTSP_SRV" "$rd" "$rf" "$rr" "$rv"
  echo "==================================================================="
  if [ "$hv" = PASS ] && [ "$rv" = PASS ]; then
    echo "RESULT: PASS (real OBS broadcast decoded over HLS + RTSP)"; exit 0
  else
    echo "RESULT: FAIL"; exit 1
  fi
fi
