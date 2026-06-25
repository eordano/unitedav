#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Unity end of the real OBS broadcast e2e. Starts mediamtx + headless OBS, launches
# the Unity editor once (headless PlayMode) against the live HLS stream, then prints
# the PlayMode verdict. Run inside the dev shell (obs/mediamtx/uav_probe on PATH):
#   nix --extra-experimental-features 'nix-command flakes' develop \
#     'path:/<repo>' -c bash tests/streaming/obs-unity-e2e.sh
set -u

STREAM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/streaming/obs-e2e.sh
source "$STREAM_DIR/obs-e2e.sh"

TESTBED="${UAV_TESTBED:-$STREAM_DIR/../unity}"
UNITY_FHS="${UAV_UNITY_FHS:-path:./unity-fhs}"
EDITOR="${UAV_UNITY_EDITOR:-$HOME/Unity/Hub/Editor/6000.4.11f1/Editor/Unity}"
RESULTS="${UAV_UNITY_RESULTS:-/tmp/obs-e2e.xml}"
EDITOR_LOG="${UAV_UNITY_LOG:-/tmp/obs-e2e.log}"
TEST_FILTER="${UAV_UNITY_FILTER:-UnitedAV.Tests.PlayMode.ObsLiveStreamTests}"

mkdir -p "$RUN_DIR"
trap 'teardown; exit 130' INT TERM

mtx_start || { teardown; exit 1; }
if ! obs_start; then teardown; exit 1; fi

log "OBS broadcasting; launching Unity editor ONCE (PlayMode)"
rm -f "$RESULTS" "$EDITOR_LOG"

nix --extra-experimental-features 'nix-command flakes' run "$UNITY_FHS" -- -c \
  "$EDITOR -batchmode -nographics -projectPath $TESTBED \
     -runTests -testPlatform PlayMode -testFilter '$TEST_FILTER' \
     -testResults $RESULTS -logFile $EDITOR_LOG"
UNITY_RC=$?

teardown

echo
echo "==================================================================="
echo "UNITY PLAYMODE e2e (live OBS HLS)"
echo "  editor rc=$UNITY_RC  results=$RESULTS  log=$EDITOR_LOG"
if [ -f "$RESULTS" ]; then
  grep -m1 "<test-run" "$RESULTS" | sed -nE 's/.*(total="[0-9]+").*(passed="[0-9]+").*(failed="[0-9]+").*(inconclusive="[0-9]+"|skipped="[0-9]+").*/  \1 \2 \3 \4/p'
  if grep -q 'result="Passed"' "$RESULTS" && ! grep -q 'result="Failed"' "$RESULTS"; then
    echo "RESULT: PASS"; STATUS=0
  elif grep -q 'result="Failed"' "$RESULTS"; then
    echo "RESULT: FAIL"; STATUS=1
  else
    echo "RESULT: INCONCLUSIVE (test ignored — was the broadcast live?)"; STATUS=2
  fi
else
  echo "RESULT: NO RESULTS FILE (editor did not produce $RESULTS)"; STATUS=1
fi
echo "==================================================================="
exit "$STATUS"
