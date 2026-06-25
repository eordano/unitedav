#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Shared Player-run reporter (sourced by run-mac.sh / run-linux.sh). Derives a
# verdict from the battery's exit code: 0 green, 2 partial, 1 failed; no JSON ->
# partial if the Player exited 0 else failed. Exits 0 for green/partial, 1 failed.

uav_report_run() {
  local target="$1" rc="$2" log="$3" json="$4"

  uav_section "Run report: $target"
  uav_log "player exit code: $rc"
  uav_log "player log      : $log"

  local backend=""
  if [ -f "$json" ]; then
    backend="$(grep -oE '"hwBackend"[[:space:]]*:[[:space:]]*"[^"]*"' "$json" | head -1 | sed -E 's/.*"([^"]*)"$/\1/')"
  fi
  [ -z "$backend" ] && [ -f "$log" ] && backend="$(uav_hw_backend_from_log "$log" || true)"
  if [ -n "$backend" ]; then
    uav_log "HW backend      : $backend  (\"[uav] hardware decode enabled: $backend\")"
  else
    uav_log "HW backend      : NOT confirmed (no hwBackend in JSON, no '[uav] hardware decode enabled' line in the log)"
  fi

  local overall="" exitcode="" pass="" fail="" skip=""
  if [ -f "$json" ]; then
    uav_log "battery report  : $json"
    sed 's/^/  [battery] /' "$json" 2>/dev/null || true
    overall="$(grep -oE '"overallPass"[[:space:]]*:[[:space:]]*(true|false)' "$json" | head -1 | grep -oE '(true|false)')"
    exitcode="$(grep -oE '"exitCode"[[:space:]]*:[[:space:]]*[0-9]+'  "$json" | head -1 | grep -oE '[0-9]+')"
    pass="$(grep -oE '"passCount"[[:space:]]*:[[:space:]]*[0-9]+'     "$json" | head -1 | grep -oE '[0-9]+')"
    fail="$(grep -oE '"failCount"[[:space:]]*:[[:space:]]*[0-9]+'     "$json" | head -1 | grep -oE '[0-9]+')"
    skip="$(grep -oE '"skipCount"[[:space:]]*:[[:space:]]*[0-9]+'     "$json" | head -1 | grep -oE '[0-9]+')"
  else
    uav_log "battery report  : not written (set UAV_E2E_REPORT; the Player may have crashed before Finalize)"
  fi

  # The battery's own exitCode is authoritative when present; else fall back to
  # the JSON tallies, then to the Player process rc.
  local effective_exit="$exitcode"
  [ -z "$effective_exit" ] && effective_exit="$rc"

  local verdict="failed"
  case "$effective_exit" in
    0)
      if [ "$overall" = "true" ]; then verdict="green"
      elif [ -n "$pass" ] && [ "${pass:-0}" -gt 0 ] && [ "${fail:-0}" -eq 0 ] && [ "${skip:-0}" -eq 0 ]; then verdict="green"
      elif [ ! -f "$json" ]; then verdict="partial"
      else verdict="partial"
      fi
      ;;
    2) verdict="partial" ;;
    *) verdict="failed"  ;;
  esac

  uav_log "tallies         : pass=${pass:-?} fail=${fail:-?} skip=${skip:-?} overallPass=${overall:-?} battery_exit=${exitcode:-n/a}"
  uav_log "VERDICT         : $verdict"
  case "$verdict" in
    green)   printf 'UNITY-RUN: %s green (pass=%s)\n'            "$target" "${pass:-?}"; return 0 ;;
    partial) printf 'UNITY-RUN: %s partial (pass=%s skip=%s)\n' "$target" "${pass:-?}" "${skip:-?}"; return 0 ;;
    *)       printf 'UNITY-FAIL: %s run failed (fail=%s exit=%s)\n' "$target" "${fail:-?}" "$effective_exit"; return 1 ;;
  esac
}
