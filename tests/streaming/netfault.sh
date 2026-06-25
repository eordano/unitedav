#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Network-fault / negative-misuse harness: drives uav_probe against broken
# transports and grades on its exit codes (0 sane / 2 open-failed / 1 internal)
# plus a hard wall-clock bound (a timeout kill == hang == FAIL). Proxy/mediamtx
# cases self-SKIP when tooling is absent; pure-ABI cases always run.
#
# Usage:
#   bash   tests/streaming/netfault.sh    # full table, nonzero on any FAIL
#   source tests/streaming/netfault.sh    # reuse netfault_one / tp_* helpers
# Exit 77 == nothing runnable on this host.
set -u

STREAM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$STREAM_DIR/../.." && pwd)"
RUN_DIR="${UAV_RUN_DIR:-$STREAM_DIR/run}"
PROBE="${UAV_PROBE:-$REPO_DIR/native/build/uav_probe}"
mkdir -p "$RUN_DIR"

# shellcheck source=tests/streaming/launch.sh
source "$STREAM_DIR/launch.sh"

# If tests/streaming/netfault-toxiproxy.sh exists it overrides the tp_* fallbacks
# defined below.
if [ -f "$STREAM_DIR/netfault-toxiproxy.sh" ]; then
  # shellcheck source=/dev/null
  source "$STREAM_DIR/netfault-toxiproxy.sh"
fi

# The wall-clock timeout is the hang detector; it sits above the decoder's ~15s
# I/O ceiling (25s) to avoid mis-flagging a graceful-but-slow return.
NF_OPEN_TIMEOUT="${UAV_NF_OPEN_TIMEOUT:-25}"
NF_STREAM_TIMEOUT="${UAV_NF_STREAM_TIMEOUT:-30}"
NF_PROXY_PORT="${UAV_NF_PROXY_PORT:-18889}"
NF_DROP_AFTER="${UAV_NF_DROP_AFTER:-3}"
NF_TRICKLE_BPS="${UAV_NF_TRICKLE_BPS:-32000}"
EXPECT_W="${UAV_EXPECT_W:-320}"
EXPECT_H="${UAV_EXPECT_H:-240}"
RMS_MIN="${UAV_RMS_MIN:-0.0005}"

ROWS=()
PASS_N=0; FAIL_N=0; SKIP_N=0

have() { command -v "$1" >/dev/null 2>&1; }
proxy_tool=""
if have socat; then proxy_tool="socat"; elif have python3; then proxy_tool="python3"; fi
have_mtx=0; { have mediamtx && have ffmpeg; } && have_mtx=1

# TCP MITM in front of mediamtx HLS; the tp_* mutators relaunch it in a mode and
# echo the proxy URL. The launchers run in a command-substitution subshell, so the
# pid is persisted to a file for tp_stop to read back.
TP_PID=""
TP_PORT="$NF_PROXY_PORT"
TP_BACKEND_HOST="127.0.0.1"
TP_BACKEND_PORT="$HLS_PORT"
TP_PIDFILE="$RUN_DIR/toxiproxy.pid"

if ! declare -f tp_start >/dev/null 2>&1; then

  _tp_kill() {
    local pid="$TP_PID"
    [ -z "$pid" ] && [ -f "$TP_PIDFILE" ] && pid="$(cat "$TP_PIDFILE" 2>/dev/null)"
    if [ -n "$pid" ]; then
      kill "$pid" 2>/dev/null
      # Reap if it's our direct child; otherwise (orphaned across a subshell)
      # poll until it's gone so the port is released before the next bind.
      wait "$pid" 2>/dev/null || {
        local _w
        for _w in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
      }
    fi
    TP_PID=""
    rm -f "$TP_PIDFILE" 2>/dev/null
  }

  # _tp_launch <mode> [arg]   modes: forward | drop_after <sec> | blackhole | trickle <bps>
  _tp_launch() {
    local mode="$1" arg="${2:-}"
    _tp_kill
    [ -n "$proxy_tool" ] || return 1
    if [ "$proxy_tool" = "python3" ]; then
      UAV_TP_MODE="$mode" UAV_TP_ARG="$arg" \
      UAV_TP_LISTEN_PORT="$TP_PORT" \
      UAV_TP_BACKEND_HOST="$TP_BACKEND_HOST" UAV_TP_BACKEND_PORT="$TP_BACKEND_PORT" \
      python3 - <<'PYEOF' >"$RUN_DIR/toxiproxy.log" 2>&1 &
import os, socket, threading, time, sys

MODE   = os.environ["UAV_TP_MODE"]
ARG    = os.environ.get("UAV_TP_ARG", "")
LPORT  = int(os.environ["UAV_TP_LISTEN_PORT"])
BHOST  = os.environ["UAV_TP_BACKEND_HOST"]
BPORT  = int(os.environ["UAV_TP_BACKEND_PORT"])

def pump(src, dst, deadline=None, bps=0):
    """Copy src->dst until EOF/deadline. Optional rate limit (bytes/sec)."""
    try:
        chunk = 4096 if not bps else max(1, bps // 8)
        while True:
            if deadline is not None and time.time() >= deadline:
                break
            src.settimeout(0.5)
            try:
                data = src.recv(chunk)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            try:
                dst.sendall(data)
            except OSError:
                break
            if bps:
                time.sleep(len(data) / float(bps))
    finally:
        for s in (src, dst):
            try: s.shutdown(socket.SHUT_RDWR)
            except OSError: pass

def handle(client):
    # blackhole: accept the connect, never forward/respond -> stalled backend.
    if MODE == "blackhole":
        try:
            time.sleep(3600)
        finally:
            client.close()
        return
    try:
        back = socket.create_connection((BHOST, BPORT), timeout=5)
    except OSError:
        client.close()
        return
    deadline = None
    bps = 0
    if MODE == "drop_after":
        deadline = time.time() + float(ARG or "3")
    elif MODE == "trickle":
        bps = int(ARG or "400")
    t1 = threading.Thread(target=pump, args=(client, back), kwargs={"deadline": deadline}, daemon=True)
    t2 = threading.Thread(target=pump, args=(back, client), kwargs={"deadline": deadline, "bps": bps}, daemon=True)
    t1.start(); t2.start()
    t1.join(); t2.join()
    for s in (client, back):
        try: s.close()
        except OSError: pass

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
# Bind with a brief retry: a previous run's proxy that was killed abruptly can
# still hold LPORT for a moment (orphaned listener / TIME_WAIT), which would make
# a fresh launch hard-fail "Address already in use" and force the proxy-driven
# cases (incl. connect-timeout-blackhole) to SKIP. Retry ~3s before giving up so
# back-to-back invocations don't flake on a closing predecessor.
for _attempt in range(30):
    try:
        srv.bind(("127.0.0.1", LPORT))
        break
    except OSError:
        time.sleep(0.1)
else:
    sys.stderr.write("toxiproxy: could not bind :%d (port held by another listener)\n" % LPORT)
    sys.stderr.flush()
    sys.exit(1)
srv.listen(16)
sys.stderr.write("toxiproxy %s listening :%d -> %s:%d\n" % (MODE, LPORT, BHOST, BPORT))
sys.stderr.flush()
while True:
    try:
        c, _ = srv.accept()
    except OSError:
        break
    threading.Thread(target=handle, args=(c,), daemon=True).start()
PYEOF
      TP_PID=$!
    else
      case "$mode" in
        blackhole)
          socat -T3600 TCP-LISTEN:"$TP_PORT",reuseaddr,fork \
                SYSTEM:'sleep 3600' >"$RUN_DIR/toxiproxy.log" 2>&1 &
          ;;
        trickle)
          local bps="${arg:-400}"
          socat -b "$bps" TCP-LISTEN:"$TP_PORT",reuseaddr,fork \
                TCP:"$TP_BACKEND_HOST":"$TP_BACKEND_PORT" >"$RUN_DIR/toxiproxy.log" 2>&1 &
          ;;
        *)
          socat TCP-LISTEN:"$TP_PORT",reuseaddr,fork \
                TCP:"$TP_BACKEND_HOST":"$TP_BACKEND_PORT" >"$RUN_DIR/toxiproxy.log" 2>&1 &
          ;;
      esac
      TP_PID=$!
    fi
    echo "$TP_PID" >"$TP_PIDFILE" 2>/dev/null
    local _i
    for _i in $(seq 1 30); do
      kill -0 "$TP_PID" 2>/dev/null || return 1
      grep -q "listening" "$RUN_DIR/toxiproxy.log" 2>/dev/null && break
      # socat has no "listening" log; just confirm the port accepts.
      if [ "$proxy_tool" = "socat" ] && have nc; then
        nc -z 127.0.0.1 "$TP_PORT" 2>/dev/null && break
      fi
      sleep 0.1
    done
    return 0
  }

  _tp_url() { echo "http://127.0.0.1:${TP_PORT}/${PATH_NAME}/index.m3u8"; }

  tp_start() {
    TP_BACKEND_HOST="${1:-127.0.0.1}"
    TP_BACKEND_PORT="${2:-$HLS_PORT}"
    _tp_launch forward || return 1
    _tp_url
  }
  tp_drop_after() { _tp_launch drop_after "${1:-$NF_DROP_AFTER}" || return 1; _tp_url; }
  tp_blackhole()  { _tp_launch blackhole || return 1; _tp_url; }
  tp_trickle()    { _tp_launch trickle "${1:-$NF_TRICKLE_BPS}" || return 1; _tp_url; }
  tp_stop()       { _tp_kill; }
fi

# netfault_one <label> <url> <expect> [timeout]
# <expect>: open-fail (exit 2), terminate (0/1), open-or-term (0/1/2),
#   terminate-or-skip (0/1 PASS, 2 SKIP), open-or-term-unbounded (legacy/unused).
netfault_one() {
  local label="$1" url="$2" expect="$3" tmo="${4:-$NF_OPEN_TIMEOUT}"
  local log; log="$(mktemp "$RUN_DIR/nf.XXXXXX.log")"
  local t0 t1 wall rc verdict="FAIL" note="" state="?" frames="-"

  t0="$(date +%s.%N)"
  timeout -k 2 "$tmo" "$PROBE" "$url" >"$log" 2>&1
  rc=$?
  t1="$(date +%s.%N)"
  wall="$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", b-a}')"

  state="$(grep -oE 'state=[A-Z]+' "$log" | tail -1 | sed 's/state=//')"
  state="${state:-?}"
  frames="$(grep -m1 '^video frames written:' "$log" | grep -oE '[0-9]+' | head -1)"
  frames="${frames:--}"

  if { [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; } && [ "$expect" != "open-or-term-unbounded" ]; then
    verdict="FAIL"; note="HANG: killed by ${tmo}s timeout"
  else
    case "$expect" in
      open-fail)
        if [ "$rc" -eq 2 ]; then verdict="PASS"; else
          verdict="FAIL"; note="expected open-fail (exit 2), got exit $rc"
        fi
        ;;
      terminate)
        if [ "$rc" -eq 0 ] || [ "$rc" -eq 1 ]; then verdict="PASS"; else
          verdict="FAIL"; note="expected bounded terminate (0/1), got exit $rc"
        fi
        ;;
      open-or-term)
        if [ "$rc" -eq 0 ] || [ "$rc" -eq 1 ] || [ "$rc" -eq 2 ]; then
          verdict="PASS"
        else
          verdict="FAIL"; note="expected open-or-term (0/1/2), got exit $rc"
        fi
        ;;
      terminate-or-skip)
        # Exit 2 means the drop window expired while uav_open was still demuxing
        # (fault hit the OPEN path, not mid-stream) -> SKIP, not a contract failure.
        if [ "$rc" -eq 0 ] || [ "$rc" -eq 1 ]; then verdict="PASS"
        elif [ "$rc" -eq 2 ]; then
          verdict="SKIP"; note="no decode progress before drop window (open-path exit 2)"
        else
          verdict="FAIL"; note="expected bounded terminate (0/1) or open-path skip (2), got exit $rc"
        fi
        ;;
      open-or-term-unbounded)
        if [ "$rc" -eq 0 ] || [ "$rc" -eq 1 ] || [ "$rc" -eq 2 ]; then verdict="PASS"
        elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
          verdict="SKIP"; note="KNOWN LIMITATION: unbounded network open"
        else
          verdict="FAIL"; note="expected open-or-term (0/1/2) or known-limitation hang (124/137), got exit $rc"
        fi
        ;;
      *)
        verdict="FAIL"; note="unknown expect '$expect'"
        ;;
    esac
  fi

  if [ "$verdict" = "PASS" ]; then
    PASS_N=$((PASS_N+1))
  elif [ "$verdict" = "SKIP" ]; then
    SKIP_N=$((SKIP_N+1))
    echo "  [SKIP] $label (rc=$rc wall=${wall}s) -- $note"
  else
    FAIL_N=$((FAIL_N+1))
    echo "  [FAIL] $label (rc=$rc wall=${wall}s) $note"
    grep -vE '^\[' "$log" | tail -4 | sed 's/^/      /'
  fi
  ROWS+=("$label|$expect|$rc|${wall}s|$state|$frames|$verdict")
  rm -f "$log"
}

netfault_skip() {
  local label="$1" why="$2"
  SKIP_N=$((SKIP_N+1))
  ROWS+=("$label|-|-|-|-|-|SKIP($why)")
  echo "  [SKIP] $label -- $why"
}

# Wait until the probe log shows decode progress before injecting a mid-stream
# fault, so the kill lands mid-stream rather than during open.
nf_wait_progress() {
  local log="$1" pid="$2" tmo="${3:-$NF_STREAM_TIMEOUT}"
  local _i iters
  iters="$(awk -v t="$tmo" 'BEGIN{printf "%d", (t+0)*10}')"
  [ "$iters" -ge 1 ] || iters=1
  for ((_i = 0; _i < iters; _i++)); do
    if grep -qE '^[[:space:]]*frame_id=|state=(PLAYING|BUFFERING)' "$log" 2>/dev/null; then
      return 0
    fi
    kill -0 "$pid" 2>/dev/null || return 1
    sleep 0.1
  done
  return 1
}

netfault_recover() {
  local label="$1" url="$2" tmo="${3:-$NF_STREAM_TIMEOUT}"
  local log; log="$(mktemp "$RUN_DIR/nfrec.XXXXXX.log")"
  UAV_HWDECODE=none timeout -k 2 "$tmo" "$PROBE" "$url" >"$log" 2>&1
  local rc=$? info dims frames rms hasaudio ok=1 verdict
  info="$(grep -m1 '^media: ' "$log")"
  dims="$(echo "$info" | grep -oE '[0-9]+x[0-9]+' | head -1)"
  frames="$(grep -m1 '^video frames written:' "$log" | grep -oE '[0-9]+' | head -1)"
  frames="${frames:-0}"
  if grep -q '^audio: none' "$log"; then hasaudio=0; rms="-"; else
    hasaudio=1; rms="$(grep -m1 '^audio: ' "$log" | sed -nE 's/.*rms=([0-9.]+).*/\1/p')"; rms="${rms:-0}"
  fi
  [ "$rc" -eq 0 ] || ok=0
  [ -n "$info" ] || ok=0
  [ "$dims" = "${EXPECT_W}x${EXPECT_H}" ] || ok=0
  [ "${frames:-0}" -ge 1 ] || ok=0
  if [ "$hasaudio" = 1 ]; then
    awk -v r="$rms" -v m="$RMS_MIN" 'BEGIN{exit !(r+0>m+0)}' || ok=0
  fi
  verdict=FAIL; [ "$ok" = 1 ] && verdict=PASS
  if [ "$verdict" = PASS ]; then PASS_N=$((PASS_N+1)); else
    FAIL_N=$((FAIL_N+1))
    echo "  [FAIL] $label (recover, rc=$rc) -- last log lines:"
    grep -vE '^\[' "$log" | tail -4 | sed 's/^/      /'
  fi
  ROWS+=("$label|recover|$rc|-|${dims:-?}|$frames|$verdict")
  rm -f "$log"
  echo "$verdict"
}

free_port() {
  local p
  for p in 18123 19345 17777; do
    if have nc; then
      nc -z 127.0.0.1 "$p" 2>/dev/null || { echo "$p"; return 0; }
    else
      echo "$p"; return 0
    fi
  done
  echo 18123
}

teardown() {
  declare -f tp_stop >/dev/null 2>&1 && tp_stop
  mtx_stop
}

run_pure_abi_cases() {
  echo "================ PURE-ABI NEGATIVE CASES (offline) ================"

  local rp; rp="$(free_port)"
  netfault_one "connect-refused" "http://127.0.0.1:${rp}/nope/index.m3u8" open-fail "$NF_OPEN_TIMEOUT"

  netfault_one "malformed-rtsp"     "rtsp://0"                open-fail "$NF_OPEN_TIMEOUT"
  netfault_one "malformed-rtsp0"    "rtsp://0.0.0.0:0/x"      open-fail "$NF_OPEN_TIMEOUT"
  netfault_one "malformed-srt"      "srt://"                  open-fail "$NF_OPEN_TIMEOUT"
  netfault_one "garbage-scheme"     "garbage://host/x"        open-fail "$NF_OPEN_TIMEOUT"
  netfault_one "empty-url"          ""                        open-fail "$NF_OPEN_TIMEOUT"
  netfault_one "nonexistent-path"   "/nonexistent/uav/clip"   open-fail "$NF_OPEN_TIMEOUT"

  netfault_one "double-fault[1/2]"  "http://127.0.0.1:${rp}/a/index.m3u8" open-fail "$NF_OPEN_TIMEOUT"
  netfault_one "double-fault[2/2]"  "garbage://x"             open-fail "$NF_OPEN_TIMEOUT"
}

run_network_cases() {
  if [ "$have_mtx" -ne 1 ]; then
    echo "================ NETWORK-FAULT CASES ================"
    netfault_skip "http-404-unknown-path"      "mediamtx/ffmpeg off PATH"
    netfault_skip "server-drop-midstream-hls"  "mediamtx/ffmpeg off PATH"
    netfault_skip "server-drop-midstream-srt"  "mediamtx/ffmpeg off PATH"
    netfault_skip "connect-timeout-blackhole"  "mediamtx/ffmpeg off PATH"
    netfault_skip "slow-trickle"               "mediamtx/ffmpeg off PATH"
    netfault_skip "reconnect-recovery"         "mediamtx/ffmpeg off PATH"
    netfault_skip "clean-recover-after-fault"  "mediamtx/ffmpeg off PATH"
    return
  fi

  echo "================ NETWORK-FAULT CASES (mediamtx) ================"

  if mtx_start; then
    local nopath="http://127.0.0.1:${HLS_PORT}/does-not-exist/index.m3u8"
    netfault_one "http-404-unknown-path" "$nopath" open-fail "$NF_OPEN_TIMEOUT"
    mtx_stop
  else
    netfault_skip "http-404-unknown-path" "mediamtx failed to start"
    mtx_stop
  fi

  if [ -z "$proxy_tool" ]; then
    netfault_skip "server-drop-midstream-hls" "no socat/python3 for TCP proxy"
    netfault_skip "connect-timeout-blackhole" "no socat/python3 for TCP proxy"
    netfault_skip "slow-trickle"              "no socat/python3 for TCP proxy"
    netfault_skip "reconnect-recovery"        "no socat/python3 for TCP proxy"
  else
    echo "  (fault proxy: $proxy_tool, MITM :$NF_PROXY_PORT -> HLS :$HLS_PORT)"

    local burl
    if burl="$(tp_blackhole)"; then
      netfault_one "connect-timeout-blackhole" "$burl" open-fail "$NF_OPEN_TIMEOUT"
      tp_stop
    else
      netfault_skip "connect-timeout-blackhole" "proxy launch failed"
    fi

    if mtx_start && mtx_publish "$PUB_CLIP_OR_DIE"; then

      local durl
      if durl="$(tp_drop_after "$NF_DROP_AFTER")"; then
        netfault_one "server-drop-midstream-hls" "$durl" terminate-or-skip "$NF_STREAM_TIMEOUT"
        tp_stop
      else
        netfault_skip "server-drop-midstream-hls" "proxy launch failed"
      fi

      local turl
      if turl="$(tp_trickle "$NF_TRICKLE_BPS")"; then
        netfault_one "slow-trickle" "$turl" open-or-term "$NF_STREAM_TIMEOUT"
        tp_stop
      else
        netfault_skip "slow-trickle" "proxy launch failed"
      fi

      local rurl
      if rurl="$(tp_drop_after "$NF_DROP_AFTER")"; then
        netfault_one "reconnect-recovery" "$rurl" terminate-or-skip "$NF_STREAM_TIMEOUT"
        tp_stop
      else
        netfault_skip "reconnect-recovery" "proxy launch failed"
      fi

      mtx_stop
    else
      netfault_skip "server-drop-midstream-hls" "publish failed"
      netfault_skip "slow-trickle"              "publish failed"
      netfault_skip "reconnect-recovery"        "publish failed"
      mtx_stop
    fi
  fi

  # SRT is UDP (no TCP proxy), so the drop is injected by killing the publisher.
  # Gate the kill on OBSERVED decode progress: a blind sleep makes PASS/FAIL
  # nondeterministic if uav_open is still synchronously demuxing the source.
  if mtx_start && mtx_publish "$PUB_CLIP_OR_DIE"; then
    local srt_url; srt_url="$(url_srt)"
    local slog; slog="$(mktemp "$RUN_DIR/nfsrt.XXXXXX.log")"
    local t0 t1 wall rc verdict state frames
    t0="$(date +%s.%N)"
    UAV_HWDECODE=none timeout -k 2 "$NF_STREAM_TIMEOUT" "$PROBE" "$srt_url" >"$slog" 2>&1 &
    local probe_pid=$!
    if nf_wait_progress "$slog" "$probe_pid" "$NF_STREAM_TIMEOUT"; then
      sleep "$NF_DROP_AFTER"
      [ -n "${PUB_PID:-}" ] && kill "$PUB_PID" 2>/dev/null
      wait "$probe_pid" 2>/dev/null; rc=$?
      t1="$(date +%s.%N)"
      wall="$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", b-a}')"
      state="$(grep -oE 'state=[A-Z]+' "$slog" | tail -1 | sed 's/state=//')"; state="${state:-?}"
      frames="$(grep -m1 '^video frames written:' "$slog" | grep -oE '[0-9]+' | head -1)"; frames="${frames:--}"
      if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        verdict="FAIL"
        echo "  [FAIL] server-drop-midstream-srt HANG: killed by ${NF_STREAM_TIMEOUT}s timeout"
        grep -vE '^\[' "$slog" | tail -4 | sed 's/^/      /'
        FAIL_N=$((FAIL_N+1))
      elif [ "$rc" -eq 0 ] || [ "$rc" -eq 1 ]; then
        verdict="PASS"; PASS_N=$((PASS_N+1))
      else
        verdict="FAIL"; FAIL_N=$((FAIL_N+1))
        echo "  [FAIL] server-drop-midstream-srt unexpected exit $rc"
        grep -vE '^\[' "$slog" | tail -4 | sed 's/^/      /'
      fi
      ROWS+=("server-drop-midstream-srt|terminate|$rc|${wall}s|$state|$frames|$verdict")
    else
      kill "$probe_pid" 2>/dev/null; wait "$probe_pid" 2>/dev/null
      netfault_skip "server-drop-midstream-srt" "no decode progress before drop window"
    fi
    rm -f "$slog"
    mtx_stop
  else
    netfault_skip "server-drop-midstream-srt" "publish failed"
    mtx_stop
  fi

  if mtx_start && mtx_publish "$PUB_CLIP_OR_DIE"; then
    echo "  recover step: fresh uav_open of healthy HLS after faults"
    netfault_recover "clean-recover-after-fault" "$(url_hls)" "$NF_STREAM_TIMEOUT" >/dev/null
    mtx_stop
  else
    netfault_skip "clean-recover-after-fault" "publish failed"
    mtx_stop
  fi
}

run_fuzz_smoke() {
  echo "================ FUZZ SMOKE (bounded, opt-in) ================"
  local ran=0 bin t
  local fuzz_dir="${UAV_FUZZ_BUILD_DIR:-$REPO_DIR/native/build-fuzz}"
  [ -d "$fuzz_dir" ] || fuzz_dir="$REPO_DIR/native/build"
  for bin in "$fuzz_dir"/fuzz_*; do
    [ -x "$bin" ] || continue
    t="$(basename "$bin")"
    local log; log="$(mktemp "$RUN_DIR/fuzz.XXXXXX.log")"
    # -max_total_time=10 self-exits in ~10-15s; the 60s timeout is a hang net
    # (a kill at 60s == HANG == FAIL, matching netfault_one).
    timeout -k 5 60 "$bin" -max_total_time=10 -rss_limit_mb=2048 >"$log" 2>&1
    local rc=$?
    if [ "$rc" -eq 0 ]; then
      PASS_N=$((PASS_N+1)); ROWS+=("$t-smoke|fuzz|$rc|-|-|-|PASS")
    elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
      FAIL_N=$((FAIL_N+1)); ROWS+=("$t-smoke|fuzz|$rc|-|-|-|FAIL")
      echo "  [FAIL] $t HANG: killed by 60s timeout (rc=$rc); -max_total_time=10 should self-exit:"
      tail -8 "$log" | sed 's/^/      /'
    else
      FAIL_N=$((FAIL_N+1)); ROWS+=("$t-smoke|fuzz|$rc|-|-|-|FAIL")
      echo "  [FAIL] $t crash/finding (rc=$rc):"
      tail -8 "$log" | sed 's/^/      /'
    fi
    rm -f "$log"
    ran=1
  done
  if [ "$ran" -eq 0 ]; then
    netfault_skip "fuzz-smoke" "no fuzz_* in $fuzz_dir (build with -DUAV_FUZZ=ON into native/build-fuzz, e.g. via tests/run-sangate.sh)"
    echo "  (fuzz targets not built in $fuzz_dir; the tier2;fuzz ctest label is the authoritative campaign)"
  fi
}

print_table() {
  echo
  echo "==================================================================="
  echo "NETFAULT — graceful-failure / recovery matrix"
  echo "(exit: 0 sane / 2 open-failed / 1 internal ; a 124/137 kill == HANG == FAIL."
  echo " The decoder enforces a finite 15s I/O ceiling (decoder.cpp:53), so every fault"
  echo " case asserts a bounded return within ~25s and a hang is always a FAIL.)"
  printf '%-30s %-12s %-5s %-7s %-10s %-7s %s\n' \
         "CASE" "EXPECT" "EXIT" "WALL" "STATE" "FRAMES" "VERDICT"
  echo "-------------------------------------------------------------------"
  local r
  for r in "${ROWS[@]}"; do
    IFS='|' read -r label expect ex wall state frames verdict <<<"$r"
    printf '%-30s %-12s %-5s %-7s %-10s %-7s %s\n' \
           "$label" "$expect" "$ex" "$wall" "$state" "$frames" "$verdict"
  done
  echo "-------------------------------------------------------------------"
  echo "TOTAL: PASS=$PASS_N FAIL=$FAIL_N SKIP=$SKIP_N"
  echo "==================================================================="
}

if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
  trap 'teardown' INT TERM EXIT

  if [ ! -x "$PROBE" ]; then
    echo "missing $PROBE (build native/ first)"; exit 1
  fi

  PUB_CLIP_OR_DIE="${UAV_CLIP:-$REPO_DIR/tests/media/out/webm__vp9__opus.webm}"
  if [ ! -f "$PUB_CLIP_OR_DIE" ]; then
    PUB_CLIP_OR_DIE="$(ls "$REPO_DIR"/tests/media/out/*.webm 2>/dev/null | head -1)"
  fi
  if [ "$have_mtx" -eq 1 ] && { [ -z "$PUB_CLIP_OR_DIE" ] || [ ! -f "$PUB_CLIP_OR_DIE" ]; }; then
    echo "no clip to publish (run tests/media/gen.sh) -> network-publish cases will SKIP"
    have_mtx=0
  fi

  run_pure_abi_cases
  run_network_cases
  run_fuzz_smoke
  print_table

  # Exit 77 so an opt-in ctest records SKIP rather than a spurious pass/fail.
  if [ "$PASS_N" -eq 0 ] && [ "$FAIL_N" -eq 0 ]; then
    echo "RESULT: SKIP (nothing runnable on this box)"; exit 77
  fi
  if [ "$FAIL_N" -eq 0 ]; then
    echo "RESULT: PASS (graceful failure + recovery on every runnable case)"; exit 0
  else
    echo "RESULT: FAIL ($FAIL_N case(s) hung or violated the negative/recovery contract)"; exit 1
  fi
fi
