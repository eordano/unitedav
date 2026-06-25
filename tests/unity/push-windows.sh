#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Push the Windows Player into a Windows VM and run its battery. Enforces a safety
# gate (a VM-monitor screendump) and proceeds only on a bare desktop / foreground
# terminal; stages the Player + run-windows.ps1 + an H.264 fixture and serves them
# over a local HTTP channel for the guest to fetch and run.
#
# Usage: bash tests/unity/push-windows.sh
# Env:
#   UAV_VM_MONITOR   VM monitor socket (default below)
#   UAV_HTTP_ADDR    host HTTP bind addr (default 127.0.0.1:8100)
#   UAV_HTTP_SCRIPT  static-file server (default /tmp/uavhttp.py)
#   UAV_FORCE_UNSAFE 1 to bypass the screendump gate (not recommended)
set -u
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/unity/common.sh
source "$SELF_DIR/common.sh"

VM_MONITOR="${UAV_VM_MONITOR:-/var/run/uav-winvm/monitor.sock}"
HTTP_ADDR="${UAV_HTTP_ADDR:-127.0.0.1:8100}"
HTTP_SCRIPT="${UAV_HTTP_SCRIPT:-/tmp/uavhttp.py}"
GUEST_HOST_ADDR="${UAV_GUEST_HOST_ADDR:-HOST_GATEWAY}"

uav_section "Push Windows Player to a Windows VM"

WIN_PLAYER_DIR="$PLAYER_WINDOWS_DIR"
EXE="$(find "$WIN_PLAYER_DIR" -maxdepth 2 -name '*.exe' 2>/dev/null | head -1)"
[ -n "$EXE" ] && [ -f "$EXE" ] || uav_skip "Windows Player .exe not found under $WIN_PLAYER_DIR. Cross-build it (build-on-mac.sh) and copy the StandaloneWindows64 output back here first."
uav_log "windows player  : $EXE"

DATA_DIR="${EXE%.exe}_Data"
if ! find "$DATA_DIR" -name 'UnitedAV.dll' 2>/dev/null | grep -q . ; then
  uav_blocker "UnitedAV.dll not found beside the Player ($DATA_DIR). The guest run will DllNotFound. Gather the Windows .dll before the cross-build (build-on-mac.sh step 2)."
fi

FIXTURE="$(uav_ensure_h264 || true)"
[ -n "$FIXTURE" ] || uav_skip "No H.264 fixture to push (set UAV_H264_FIXTURE or run tests/media/gen.sh)."
uav_log "h264 fixture    : $FIXTURE"

# MANDATORY SAFETY GATE: screendump the guest; refuse to drive a live session.
safety_gate() {
  if [ "${UAV_FORCE_UNSAFE:-0}" = "1" ]; then
    uav_blocker "UAV_FORCE_UNSAFE=1 — bypassing the screendump safety gate. Only do this on a bare/disposable desktop."
    return 0
  fi
  if [ ! -S "$VM_MONITOR" ]; then
    uav_skip "VM monitor socket $VM_MONITOR not present — cannot screendump to verify the guest is safe to drive. Start the VM with a monitor socket (or set UAV_VM_MONITOR), or run run-windows.ps1 manually in the guest."
  fi
  command -v socat >/dev/null 2>&1 || uav_skip "socat not found — needed to talk to the VM monitor for the safety screendump. Install socat or run the guest steps manually."

  local shot; shot="$PLAYERS_DIR/run/windows/guard-$(date +%s).ppm"
  mkdir -p "$(dirname "$shot")"
  uav_log "safety gate     : screendump -> $shot"
  printf 'screendump %s\n' "$shot" | socat - "UNIX-CONNECT:$VM_MONITOR" >/dev/null 2>&1 || \
    uav_skip "screendump failed over $VM_MONITOR. Cannot verify the guest is safe to drive; refusing to proceed."
  local _; for _ in $(seq 1 50); do [ -s "$shot" ] && break; done
  [ -s "$shot" ] || uav_skip "screendump produced no image ($shot). Refusing to drive the guest blind."

  uav_log "safety gate     : screendump captured ($(wc -c <"$shot") bytes)."
  uav_log "REVIEW REQUIRED : open $shot and confirm the guest shows a bare desktop or a foreground"
  uav_log "                  PowerShell/terminal — NOT a live application / game session."
  if [ "${UAV_GATE_CONFIRMED:-0}" != "1" ]; then
    uav_skip "Safety gate not confirmed. After eyeballing $shot, re-run with UAV_GATE_CONFIRMED=1 ONLY if a bare desktop / foreground terminal is shown. If a live session is foreground, STOP (this is the expected skip)."
  fi
  uav_log "safety gate     : confirmed by operator (UAV_GATE_CONFIRMED=1)."
}
safety_gate

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
uav_log "staging         : $STAGE"
mkdir -p "$STAGE/Player" "$STAGE/media" "$STAGE/scripts"
cp -a "$WIN_PLAYER_DIR/." "$STAGE/Player/"
cp "$FIXTURE" "$STAGE/media/"
cp "$SELF_DIR/run-windows.ps1" "$STAGE/scripts/"
PAYLOAD="$STAGE/uav-win-payload.tar"
( tar -C "$STAGE" -cf "$PAYLOAD" Player media scripts ) || uav_skip "Failed to stage the Windows payload tarball."
uav_log "payload         : $PAYLOAD ($(wc -c <"$PAYLOAD") bytes)"

if [ ! -f "$HTTP_SCRIPT" ]; then
  uav_blocker "HTTP channel server $HTTP_SCRIPT not found. The guest cannot pull the payload automatically."
  cat <<EOF
MANUAL FALLBACK (no HTTP channel):
  1. Transfer $PAYLOAD into the guest (shared folder / scp / drag-drop).
  2. In the guest PowerShell:
       mkdir C:\uav; tar -xf <payload>.tar -C C:\uav
       pwsh C:\uav\scripts\run-windows.ps1
  3. Collect C:\uav\out\battery.json + player.log for the report.
EOF
  uav_skip "Automatic push unavailable; manual steps printed above."
fi

uav_log "serving payload over http://$HTTP_ADDR/ (guest pulls via http://$GUEST_HOST_ADDR:${HTTP_ADDR##*:})"
( python3 "$HTTP_SCRIPT" "$HTTP_ADDR" "$STAGE" >"$PLAYERS_DIR/run/windows/http.log" 2>&1 ) &
HTTP_PID=$!
trap 'kill "$HTTP_PID" 2>/dev/null; rm -rf "$STAGE"' EXIT
host_port="${HTTP_ADDR}"
for _ in $(seq 1 50); do
  if curl -fsS "http://$host_port/uav-win-payload.tar" -o /dev/null 2>/dev/null; then break; fi
done

cat <<EOF

GUEST STEP (run these in the confirmed-foreground guest PowerShell; the guest
reaches the host at http://$GUEST_HOST_ADDR:${HTTP_ADDR##*:}):

  Invoke-WebRequest http://$GUEST_HOST_ADDR:${HTTP_ADDR##*:}/uav-win-payload.tar -OutFile C:\uav-win-payload.tar
  New-Item -ItemType Directory -Force C:\uav | Out-Null
  tar -xf C:\uav-win-payload.tar -C C:\uav
  pwsh C:\uav\scripts\run-windows.ps1

Then fetch the result back to the host for the report:
  Invoke-WebRequest -Uri http://$GUEST_HOST_ADDR:${HTTP_ADDR##*:}/upload -Method Put -InFile C:\uav\out\battery.json   # if the channel supports upload
  (otherwise read C:\uav\out\battery.json + player.log in the guest)

The HTTP server stays up until this script exits. Press Ctrl-C when the guest run
is done, or wire your guest agent to signal completion.
EOF

uav_log "Windows push staged + served. Drive the guest steps above (gate already confirmed)."
wait "$HTTP_PID" 2>/dev/null || true
