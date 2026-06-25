#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Build the Showcase sample as a StandaloneOSX .app (run on the macOS build host).
# Stages the (hidden) Samples~/Showcase into the test project's Assets so
# BuildShowcase + ShowcaseController compile, runs the build, then unstages.
set -u
REPO="${UAV_REPO:-/Users/usr/unitedav}"
UNITY="${UAV_UNITY_BIN:-/Applications/Unity/Unity-6000.4.11f1/Unity.app/Contents/MacOS/Unity}"
PROJ="$REPO/tests/unity"
OUT="${UAV_SHOWCASE_OUT:-$PROJ/Build/showcase-osx/Showcase.app}"
LOG="$PROJ/build-showcase.log"
STAGE="$PROJ/Assets/_Showcase"

[ -x "$UNITY" ] || { echo "FAIL: Unity editor not found at $UNITY"; exit 1; }
[ -f "$REPO/unity/Runtime/Plugins/macOS/libUnitedAV.dylib" ] || echo "WARN: macOS plugin not staged; video may be a stub."

rm -rf "$STAGE" "$STAGE.meta"; mkdir -p "$STAGE"
cp -a "$REPO/unity/Samples~/Showcase/." "$STAGE/"
rm -rf "$OUT"; mkdir -p "$(dirname "$OUT")"

echo "[showcase] building $OUT (log: $LOG)"
"$UNITY" -batchmode -nographics -quit -projectPath "$PROJ" \
  -executeMethod BuildShowcase.OSX -uavOut "$OUT" -logFile "$LOG" 2>&1
RC=$?
rm -rf "$STAGE" "$STAGE.meta"

if [ -d "$OUT" ]; then
  echo "SHOWCASE_APP=$OUT (editor rc=$RC)"
else
  echo "NO-APP (editor rc=$RC). Log tail:"; grep -iE 'error|exception|BuildResult|Aborting' "$LOG" 2>/dev/null | tail -25
  exit 2
fi
