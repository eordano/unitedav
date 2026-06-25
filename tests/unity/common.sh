#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Shared helpers for the Unity build/run scripts. Defines variables/functions only.
#
# Skip convention:
#   uav_skip <reason>     -> "UNITY-SKIP: ..." and exit 0
#   uav_blocker <reason>  -> "UNITY-BLOCKER: ..." (does not exit)
#   uav_die <reason>      -> "UNITY-FAIL: ..." and exit 1

set -u

UAV_COMMON_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$UAV_COMMON_DIR/../.." && pwd)"
export REPO_DIR

UNITY_PROJECT_DIR="${UAV_UNITY_PROJECT_DIR:-$UAV_COMMON_DIR}"
export UNITY_PROJECT_DIR

PACKAGE_DIR="$REPO_DIR/unity"
PLUGINS_DIR="$PACKAGE_DIR/Runtime/Plugins"
export PACKAGE_DIR PLUGINS_DIR

# Players + logs land outside the repo so the asset importer never ingests them.
PLAYERS_DIR="${UAV_PLAYERS_DIR:-${TMPDIR:-/tmp}/unitedav-unity-players}"
export PLAYERS_DIR

PLAYER_MAC_DIR="$PLAYERS_DIR/mac"
PLAYER_LINUX_DIR="$PLAYERS_DIR/linux"
PLAYER_WINDOWS_DIR="$PLAYERS_DIR/windows"
export PLAYER_MAC_DIR PLAYER_LINUX_DIR PLAYER_WINDOWS_DIR

# Must match the example app's productName.
PLAYER_NAME="${UAV_PLAYER_NAME:-UnitedAVEndToEnd}"
export PLAYER_NAME

UNITY_VERSION="${UAV_UNITY_VERSION:-6000.4.11f1}"
export UNITY_VERSION

# Unity Hub CLI module ids: mac-mono -> StandaloneOSX, windows-mono ->
# StandaloneWindows64, linux-mono -> StandaloneLinux64 (IL2CPP variants end -il2cpp).
UNITY_MODULES="${UAV_UNITY_MODULES:-mac-mono windows-mono linux-mono}"
export UNITY_MODULES

UNITY_BUILD_TARGETS="${UAV_UNITY_BUILD_TARGETS:-StandaloneOSX StandaloneWindows64 StandaloneLinux64}"
export UNITY_BUILD_TARGETS

# UAV_HWDECODE=auto only engages for codecs the HW supports, so the HW assertion
# must use an H.264 fixture.
export UAV_HWDECODE="${UAV_HWDECODE:-auto}"

UAV_HW_LINE_RE='\[uav\] hardware decode enabled: (vaapi|videotoolbox|cuda|d3d11va)'
export UAV_HW_LINE_RE

uav_log()     { printf '%s\n' "[unity] $*"; }
uav_section() { printf '\n========== %s ==========\n' "$*"; }
uav_skip()    { printf 'UNITY-SKIP: %s\n' "$*"; exit 0; }
uav_blocker() { printf 'UNITY-BLOCKER: %s\n' "$*"; }
uav_die()     { printf 'UNITY-FAIL: %s\n' "$*" >&2; exit 1; }

# Priority: $UAV_H264_FIXTURE, then $UAV_TEST_MEDIA_DIR, then tests/media/out.
uav_h264_fixtures=( "mp4__h264__aac.mp4" "mov__h264__aac.mov" "mpegts__h264__mp3.ts" )

uav_find_h264_in() {
  local dir="$1" name
  [ -n "$dir" ] && [ -d "$dir" ] || return 1
  for name in "${uav_h264_fixtures[@]}"; do
    if [ -f "$dir/$name" ]; then printf '%s\n' "$dir/$name"; return 0; fi
  done
  return 1
}

uav_resolve_h264() {
  if [ -n "${UAV_H264_FIXTURE:-}" ] && [ -f "${UAV_H264_FIXTURE}" ]; then
    printf '%s\n' "$UAV_H264_FIXTURE"; return 0
  fi
  local found
  if found="$(uav_find_h264_in "${UAV_TEST_MEDIA_DIR:-}")"; then
    printf '%s\n' "$found"; return 0
  fi
  if found="$(uav_find_h264_in "$REPO_DIR/tests/media/out")"; then
    printf '%s\n' "$found"; return 0
  fi
  return 1
}

uav_ensure_h264() {
  local f
  if f="$(uav_resolve_h264)"; then printf '%s\n' "$f"; return 0; fi
  if command -v ffmpeg >/dev/null 2>&1 && [ -f "$REPO_DIR/tests/media/gen.sh" ]; then
    uav_log "no H.264 fixture found; running tests/media/gen.sh" >&2
    bash "$REPO_DIR/tests/media/gen.sh" >/dev/null 2>&1 || true
    if f="$(uav_resolve_h264)"; then printf '%s\n' "$f"; return 0; fi
  fi
  return 1
}

uav_hw_backend_from_log() {
  local log="$1"
  [ -f "$log" ] || return 1
  grep -oE "$UAV_HW_LINE_RE" "$log" 2>/dev/null | head -1 | sed -E 's/.*: //'
}

uav_find_unity_editor() {
  if [ -n "${UAV_UNITY_BIN:-}" ] && [ -x "${UAV_UNITY_BIN}" ]; then
    printf '%s\n' "$UAV_UNITY_BIN"; return 0
  fi
  local c
  for c in \
    "/Applications/Unity/Hub/Editor/$UNITY_VERSION/Unity.app/Contents/MacOS/Unity" \
    "$HOME/Unity/Hub/Editor/$UNITY_VERSION/Editor/Unity" \
    "/opt/unity/editors/$UNITY_VERSION/Editor/Unity" ; do
    if [ -x "$c" ]; then printf '%s\n' "$c"; return 0; fi
  done
  if command -v Unity >/dev/null 2>&1; then command -v Unity; return 0; fi
  return 1
}

uav_find_unity_hub() {
  if [ -n "${UAV_UNITY_HUB:-}" ] && [ -x "${UAV_UNITY_HUB}" ]; then
    printf '%s\n' "$UAV_UNITY_HUB"; return 0
  fi
  local c
  for c in \
    "/Applications/Unity Hub.app/Contents/MacOS/Unity Hub" \
    "$HOME/Applications/Unity Hub.AppImage" \
    "/opt/unityhub/unityhub" ; do
    if [ -x "$c" ]; then printf '%s\n' "$c"; return 0; fi
  done
  if command -v unityhub >/dev/null 2>&1; then command -v unityhub; return 0; fi
  return 1
}
