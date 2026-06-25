#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run the Linux standalone Player locally under xvfb (real GL/Vulkan context, NOT
# -nographics): the example app's IntegrationBattery self-tests the player surface,
# writes a JSON result, and sets the exit code. Run inside the dev shell (or set
# UAV_LD_LIBRARY_PATH) so libav* resolve.
#
# Usage:
#   nix --extra-experimental-features 'nix-command flakes' develop -c \
#     bash tests/unity/run-linux.sh
set -u
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/unity/common.sh
source "$SELF_DIR/common.sh"

uav_section "Run Linux Player (local, xvfb)"
case "$(uname -s)" in
  Linux) : ;;
  *) uav_skip "run-linux.sh must run on Linux (the Linux Player's native target); this host is $(uname -s)." ;;
esac

BIN="${UAV_PLAYER_BIN:-}"
if [ -z "$BIN" ]; then
  BIN="$(find "$PLAYER_LINUX_DIR" -maxdepth 2 -type f \( -name "$PLAYER_NAME" -o -name '*.x86_64' \) -perm -111 2>/dev/null | head -1)"
fi
[ -n "$BIN" ] && [ -f "$BIN" ] || uav_skip "Linux Player not found under $PLAYER_LINUX_DIR. Cross-build it (build-on-mac.sh) and copy it back, or set UAV_PLAYER_BIN."
chmod +x "$BIN" 2>/dev/null || true
uav_log "player binary   : $BIN"

HAVE_DISPLAY=0
[ -n "${DISPLAY:-}" ] && HAVE_DISPLAY=1
XVFB="$(command -v xvfb-run || true)"
if [ "$HAVE_DISPLAY" = 0 ] && [ -z "$XVFB" ]; then
  uav_skip "No DISPLAY and xvfb-run not found. Install xvfb or run inside the dev shell, so the Linux Player gets a real GL context (do NOT use -nographics for the pixel battery)."
fi

FIXTURE="$(uav_ensure_h264 || true)"
[ -n "$FIXTURE" ] || uav_skip "No H.264 fixture (set UAV_H264_FIXTURE or run tests/media/gen.sh)."
UAV_TEST_MEDIA_DIR="$(dirname "$FIXTURE")"; export UAV_TEST_MEDIA_DIR
export UAV_H264_FIXTURE="$FIXTURE"
uav_log "h264 fixture    : $FIXTURE"
uav_log "UAV_HWDECODE    : $UAV_HWDECODE"

if [ -e /dev/dri/renderD128 ]; then
  uav_log "vaapi device    : /dev/dri/renderD128 present (HW decode possible)"
else
  uav_log "vaapi device    : no /dev/dri render node — HW decode will fall back to SW (battery still validates decode; HW line will be absent)"
fi

if [ -n "${UAV_LD_LIBRARY_PATH:-}" ]; then
  export LD_LIBRARY_PATH="${UAV_LD_LIBRARY_PATH}:${LD_LIBRARY_PATH:-}"
fi

OUT_DIR="$PLAYERS_DIR/run/linux"; mkdir -p "$OUT_DIR"
PLAYER_LOG="$OUT_DIR/player.log"
RESULT_JSON="$OUT_DIR/battery.json"
# IntegrationBattery writes its BatteryReport to UAV_E2E_REPORT.
export UAV_E2E_REPORT="$RESULT_JSON"

uav_log "running battery (one run) -> $RESULT_JSON"
if [ "$HAVE_DISPLAY" = 1 ]; then
  uav_log "using existing DISPLAY=$DISPLAY"
  "$BIN" -batchmode -logFile "$PLAYER_LOG" 2>&1 | sed 's/^/  [player] /'
  RC=${PIPESTATUS[0]}
else
  uav_log "wrapping in xvfb-run ($XVFB)"
  "$XVFB" -a -s "-screen 0 1280x1024x24" \
    "$BIN" -batchmode -logFile "$PLAYER_LOG" 2>&1 | sed 's/^/  [player] /'
  RC=${PIPESTATUS[0]}
fi

source "$SELF_DIR/_report-run.sh"
uav_report_run "linux" "$RC" "$PLAYER_LOG" "$RESULT_JSON"
