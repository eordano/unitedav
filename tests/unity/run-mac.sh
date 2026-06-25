#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run the cross-built StandaloneOSX Player on macOS headlessly: the example app's
# IntegrationBattery self-tests the player surface + HW backend, writes a JSON
# result, and sets the exit code.
#
# Usage:
#   bash tests/unity/run-mac.sh
#   UAV_PLAYER_APP=/path/to/UnitedAVEndToEnd.app bash tests/unity/run-mac.sh
set -u
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/unity/common.sh
source "$SELF_DIR/common.sh"

uav_section "Run macOS Player"
case "$(uname -s)" in
  Darwin) : ;;
  *) uav_skip "run-mac.sh must run on macOS; this host is $(uname -s). A mac Player cannot execute elsewhere." ;;
esac

APP="${UAV_PLAYER_APP:-}"
if [ -z "$APP" ]; then
  APP="$(find "$PLAYER_MAC_DIR" -maxdepth 2 -name '*.app' 2>/dev/null | head -1)"
fi
[ -n "$APP" ] && [ -d "$APP" ] || uav_skip "macOS Player (.app) not found under $PLAYER_MAC_DIR. Run tests/unity/build-on-mac.sh (BuildOnMac) first."
BIN="$(find "$APP/Contents/MacOS" -maxdepth 1 -type f -perm -111 2>/dev/null | head -1)"
[ -n "$BIN" ] || uav_skip "No executable inside $APP/Contents/MacOS."
uav_log "player app      : $APP"
uav_log "player binary   : $BIN"

FIXTURE="$(uav_ensure_h264 || true)"
[ -n "$FIXTURE" ] || uav_skip "No H.264 fixture (set UAV_H264_FIXTURE or run tests/media/gen.sh). HW decode cannot be exercised."
UAV_TEST_MEDIA_DIR="$(dirname "$FIXTURE")"; export UAV_TEST_MEDIA_DIR
export UAV_H264_FIXTURE="$FIXTURE"
uav_log "h264 fixture    : $FIXTURE"
uav_log "UAV_HWDECODE    : $UAV_HWDECODE"

OUT_DIR="$PLAYERS_DIR/run/mac"; mkdir -p "$OUT_DIR"
PLAYER_LOG="$OUT_DIR/player.log"
RESULT_JSON="$OUT_DIR/battery.json"
# IntegrationBattery writes its BatteryReport to UAV_E2E_REPORT.
export UAV_E2E_REPORT="$RESULT_JSON"

uav_log "running battery (one run) -> $RESULT_JSON"
UAV_HWDECODE="$UAV_HWDECODE" \
UAV_TEST_MEDIA_DIR="$UAV_TEST_MEDIA_DIR" \
UAV_H264_FIXTURE="$UAV_H264_FIXTURE" \
UAV_E2E_REPORT="$RESULT_JSON" \
  "$BIN" -batchmode -logFile "$PLAYER_LOG" 2>&1 | sed 's/^/  [player] /'
RC=${PIPESTATUS[0]}

source "$SELF_DIR/_report-run.sh"
uav_report_run "mac" "$RC" "$PLAYER_LOG" "$RESULT_JSON"
