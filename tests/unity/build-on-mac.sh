#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Unity build hub (run on a macOS arm64 host). Installs Standalone Build Support
# modules, gathers the 3 native plugins (macOS .dylib built here, Linux .so
# committed, Windows .dll from a Windows VM — none cross-compiled), runs EditMode +
# PlayMode tests with a real GfxDevice, and cross-builds the three Players.
#
# Usage:
#   bash tests/unity/build-on-mac.sh                 # full: modules+tests+build
#   UAV_ONLY=tests  bash tests/unity/build-on-mac.sh # only -runTests
#   UAV_ONLY=build  bash tests/unity/build-on-mac.sh # only cross-build Players
set -u
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/unity/common.sh
source "$SELF_DIR/common.sh"

ONLY="${UAV_ONLY:-all}"
BLOCKERS=()
note_blocker() { BLOCKERS+=("$1"); uav_blocker "$1"; }

uav_section "UnitedAV Unity build hub"
uav_log "repo            : $REPO_DIR"
uav_log "unity project   : $UNITY_PROJECT_DIR"
uav_log "package         : $PACKAGE_DIR"
uav_log "editor version  : $UNITY_VERSION"
uav_log "players out      : $PLAYERS_DIR"

case "$(uname -s)" in
  Darwin) : ;;
  *) uav_log "WARNING: build-on-mac.sh is intended to run on macOS. uname=$(uname -s)."
     uav_log "         Continuing anyway, but VideoToolbox HW + StandaloneOSX builds require macOS." ;;
esac

UNITY_BIN="$(uav_find_unity_editor || true)"
[ -n "$UNITY_BIN" ] || uav_skip "Unity editor $UNITY_VERSION not found (set UAV_UNITY_BIN). Install via Unity Hub."
uav_log "unity editor    : $UNITY_BIN"

[ -d "$UNITY_PROJECT_DIR" ] || uav_skip "committed Unity project missing at $UNITY_PROJECT_DIR (authored under tests/unity/project). Cannot -runTests / build without it."

install_modules() {
  uav_section "Install build-support modules"
  if [ "${UAV_SKIP_MODULE_INSTALL:-0}" = "1" ]; then
    uav_log "UAV_SKIP_MODULE_INSTALL=1 -> skipping module install."
    return 0
  fi
  local hub; hub="$(uav_find_unity_hub || true)"
  if [ -z "$hub" ]; then
    note_blocker "Unity Hub CLI not found; cannot auto-install Standalone Build Support. Install '$UNITY_MODULES' for $UNITY_VERSION manually, or pre-install the modules and re-run with UAV_SKIP_MODULE_INSTALL=1."
    return 0
  fi
  uav_log "unity hub       : $hub"
  local mod args
  args=( -- --headless install-modules --version "$UNITY_VERSION" )
  for mod in $UNITY_MODULES; do args+=( --module "$mod" ); done
  uav_log "installing modules: $UNITY_MODULES"
  if "$hub" "${args[@]}" 2>&1 | sed 's/^/  [hub] /'; then
    uav_log "module install command completed (verify per-target below)."
  else
    note_blocker "Unity Hub install-modules returned non-zero. Module ids may differ for $UNITY_VERSION (try the -il2cpp variants); inspect the [hub] output above."
  fi
}

gather_plugins() {
  uav_section "Gather native plugins"
  PLUGIN_LINUX="$PLUGINS_DIR/Linux/x86_64/libUnitedAV.so"
  PLUGIN_MAC="$PLUGINS_DIR/macOS/libUnitedAV.dylib"
  PLUGIN_WIN="$PLUGINS_DIR/Windows/x86_64/UnitedAV.dll"

  if [ -f "$PLUGIN_LINUX" ]; then
    uav_log "linux   plugin  : present ($PLUGIN_LINUX)"
  else
    note_blocker "Linux plugin missing at $PLUGIN_LINUX. Build it locally (cmake -S native -B native/build && cp native/build/libUnitedAV.so $PLUGIN_LINUX) and commit, or copy it onto this host."
  fi

  if [ -f "$PLUGIN_MAC" ]; then
    uav_log "macos   plugin  : present ($PLUGIN_MAC)"
  else
    uav_log "macos   plugin  : missing -> attempting nix-shell build"
    if build_macos_plugin; then
      uav_log "macos   plugin  : built ($PLUGIN_MAC)"
    else
      note_blocker "macOS .dylib not built. Build it: nix --extra-experimental-features 'nix-command flakes' shell <repo> -c bash -c 'cmake -S native -B native/build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build native/build-mac' then copy build-mac/libUnitedAV.dylib to $PLUGIN_MAC."
    fi
  fi
  ensure_plugin_meta "$PLUGIN_MAC"   macos
  ensure_plugin_meta "$PLUGIN_WIN"   windows

  if [ -f "$PLUGIN_WIN" ]; then
    uav_log "windows plugin  : present ($PLUGIN_WIN)"
  elif [ -n "${UAV_WIN_DLL:-}" ] && [ -f "${UAV_WIN_DLL}" ]; then
    mkdir -p "$(dirname "$PLUGIN_WIN")"
    cp "$UAV_WIN_DLL" "$PLUGIN_WIN"
    uav_log "windows plugin  : copied from UAV_WIN_DLL=$UAV_WIN_DLL"
    ensure_plugin_meta "$PLUGIN_WIN" windows
  elif [ -f "/tmp/guestout/UnitedAV.dll" ]; then
    mkdir -p "$(dirname "$PLUGIN_WIN")"
    cp "/tmp/guestout/UnitedAV.dll" "$PLUGIN_WIN"
    uav_log "windows plugin  : copied from /tmp/guestout/UnitedAV.dll"
    ensure_plugin_meta "$PLUGIN_WIN" windows
  else
    note_blocker "Windows UnitedAV.dll missing. Build it in a Windows VM, fetch it to /tmp/guestout/UnitedAV.dll (or set UAV_WIN_DLL=<path>), then re-run. The StandaloneWindows64 Player will be built without it (DllNotFound at runtime) and the windows leg is blocked."
  fi
}

# Build via `nix shell`, NOT `nix develop`: valgrind in the devshell breaks
# aarch64-darwin.
build_macos_plugin() {
  command -v nix >/dev/null 2>&1 || return 1
  local out="$REPO_DIR/native/build-mac"
  ( nix --extra-experimental-features 'nix-command flakes' shell "path:$REPO_DIR" -c \
      bash -c "cmake -S '$REPO_DIR/native' -B '$out' -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build '$out'" ) \
      2>&1 | sed 's/^/  [nix] /'
  local built="$out/libUnitedAV.dylib"
  [ -f "$built" ] || return 1
  mkdir -p "$(dirname "$PLUGIN_MAC")"
  cp "$built" "$PLUGIN_MAC"
}

# Writes a PluginImporter .meta only if the binary exists AND no .meta is present
# (never clobbers the committed Linux meta).
ensure_plugin_meta() {
  local bin="$1" platform="$2" meta="$1.meta"
  [ -f "$bin" ] || return 0
  [ -f "$meta" ] && { uav_log "meta present    : $meta"; return 0; }

  local guid; guid="$(uav_guid)"
  # macOS ships a universal/arm64 .dylib -> AnyCPU (x86_64 would hide it from an
  # Apple-Silicon editor/Player).
  local excl_osx=1 excl_win=1 excl_lin=1 editor_os="Linux" sa_first="" sa_cpu="x86_64" editor_cpu="x86_64"
  case "$platform" in
    macos)   excl_osx=0; editor_os="OSX";     sa_first="Standalone: OSXUniversal"; sa_cpu="AnyCPU"; editor_cpu="AnyCPU" ;;
    windows) excl_win=0; editor_os="Windows"; sa_first="Standalone: Win64" ;;
    linux)   excl_lin=0; editor_os="Linux";   sa_first="Standalone: Linux64" ;;
  esac
  uav_log "writing meta    : $meta ($platform)"
  cat > "$meta" <<EOF
fileFormatVersion: 2
guid: $guid
PluginImporter:
  externalObjects: {}
  serializedVersion: 2
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 0
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
  - first:
      : Any
    second:
      enabled: 0
      settings:
        Exclude Editor: 0
        Exclude Linux64: $excl_lin
        Exclude OSXUniversal: $excl_osx
        Exclude Win: $excl_win
        Exclude Win64: $excl_win
  - first:
      Any:
    second:
      enabled: 0
      settings: {}
  - first:
      Editor: Editor
    second:
      enabled: 1
      settings:
        CPU: $editor_cpu
        DefaultValueInitialized: true
        OS: $editor_os
  - first:
      $sa_first
    second:
      enabled: 1
      settings:
        CPU: $sa_cpu
  userData:
  assetBundleName:
  assetBundleVariant:
EOF
}

uav_guid() {
  if command -v uuidgen >/dev/null 2>&1; then
    uuidgen | tr -d '-' | tr 'A-Z' 'a-z'
  else
    head -c16 /dev/urandom | od -An -tx1 | tr -d ' \n'
  fi
}

run_tests() {
  uav_section "Run EditMode + PlayMode tests (real GfxDevice)"
  local fixture; fixture="$(uav_ensure_h264 || true)"
  if [ -n "$fixture" ]; then
    UAV_TEST_MEDIA_DIR="$(dirname "$fixture")"; export UAV_TEST_MEDIA_DIR
    export UAV_H264_FIXTURE="$fixture"
    uav_log "h264 fixture    : $fixture (UAV_HWDECODE=$UAV_HWDECODE)"
  else
    note_blocker "No H.264 fixture for the HW-decode assertion. The PlayMode test will self-skip the HW check (run tests/media/gen.sh to populate tests/media/out)."
  fi

  local logbase="$PLAYERS_DIR/logs"; mkdir -p "$logbase"
  local rc_edit=0 rc_play=0

  for platform in editmode playmode; do
    local results="$logbase/results-$platform.xml"
    local elog="$logbase/runtests-$platform.log"
    uav_log "running $platform -> $results"
    # No -nographics: PlayMode pixel-readback + VideoToolbox HW decode need a real
    # Metal GfxDevice.
    UAV_HWDECODE="$UAV_HWDECODE" "$UNITY_BIN" \
      -runTests \
      -batchmode \
      -projectPath "$UNITY_PROJECT_DIR" \
      -testPlatform "$platform" \
      -testResults "$results" \
      -logFile "$elog" \
      -timeout 1800 2>&1 | sed 's/^/  [unity] /'
    local rc=${PIPESTATUS[0]}
    [ "$platform" = "editmode" ] && rc_edit=$rc || rc_play=$rc
    summarize_results "$results" "$platform" "$rc"
  done

  local plog="$logbase/runtests-playmode.log"
  local backend; backend="$(uav_hw_backend_from_log "$plog" || true)"
  if [ -n "$backend" ]; then
    uav_log "HW decode CONFIRMED in Unity: $backend  (\"[uav] hardware decode enabled: $backend\")"
  else
    note_blocker "Did not observe the '[uav] hardware decode enabled: <backend>' line in $plog. The HW path may have fallen back to software (check the fixture is H.264 and a GPU/VideoToolbox is available)."
  fi

  if [ "$rc_edit" -ne 0 ] || [ "$rc_play" -ne 0 ]; then
    note_blocker "Unity -runTests reported failures (editmode rc=$rc_edit, playmode rc=$rc_play). See $logbase/results-*.xml."
    return 1
  fi
  uav_log "EditMode + PlayMode: PASS"
}

summarize_results() {
  local xml="$1" platform="$2" rc="$3"
  if [ ! -f "$xml" ]; then
    uav_log "$platform: NO RESULTS XML (editor rc=$rc) — likely a compile/launch failure (see log)."
    return
  fi
  local total passed failed skipped
  total=$(grep -oE 'total="[0-9]+"'   "$xml" | head -1 | grep -oE '[0-9]+')
  passed=$(grep -oE 'passed="[0-9]+"' "$xml" | head -1 | grep -oE '[0-9]+')
  failed=$(grep -oE 'failed="[0-9]+"' "$xml" | head -1 | grep -oE '[0-9]+')
  skipped=$(grep -oE 'skipped="[0-9]+"' "$xml" | head -1 | grep -oE '[0-9]+')
  uav_log "$platform: total=${total:-?} passed=${passed:-?} failed=${failed:-?} skipped=${skipped:-?} (editor rc=$rc)"
}

# A `Samples~` folder is hidden from Unity, so the example app is STAGED into the
# project's Assets/ transiently for the build, then removed.
STAGE_REL="Assets/_UAVEndToEndBuild"
build_players() {
  uav_section "Cross-build standalone Players"
  mkdir -p "$PLAYER_MAC_DIR" "$PLAYER_LINUX_DIR" "$PLAYER_WINDOWS_DIR"
  local logbase="$PLAYERS_DIR/logs"; mkdir -p "$logbase"
  local blog="$logbase/build-players.log"

  local sample_src="$PACKAGE_DIR/Samples~/EndToEnd"
  if [ ! -f "$sample_src/EndToEnd.unity" ]; then
    note_blocker "Example app scene missing at $sample_src/EndToEnd.unity (authored under unity/Samples~/EndToEnd by the example-app component). Cannot cross-build Players."
    return 0
  fi

  local stage_abs="$UNITY_PROJECT_DIR/$STAGE_REL"
  stage_example "$sample_src" "$stage_abs" || { note_blocker "Failed to stage the example app into the project."; return 0; }
  trap 'cleanup_stage "$stage_abs"' RETURN

  uav_log "build targets   : $UNITY_BUILD_TARGETS"
  uav_log "build method    : UnitedAV.Build.PlayerBuilder.BuildAll (generated Editor script, transient)"
  UAV_BUILD_TARGETS="$UNITY_BUILD_TARGETS" \
  UAV_BUILD_OUTPUT="$PLAYERS_DIR" \
  UAV_PLAYER_NAME="$PLAYER_NAME" \
    "$UNITY_BIN" \
      -batchmode -quit -nographics \
      -projectPath "$UNITY_PROJECT_DIR" \
      -executeMethod UnitedAV.Build.PlayerBuilder.BuildAll \
      -logFile "$blog" 2>&1 | sed 's/^/  [build] /'
  local rc=${PIPESTATUS[0]}

  if [ "$rc" -ne 0 ]; then
    note_blocker "Player cross-build returned rc=$rc. Common causes: 'BuildTarget … not installed' (the matching Standalone Build Support module did not install — see step 1), or a compile error in the staged example app. Log: $blog."
  fi

  # Artifact presence is the source of truth, not rc.
  report_player mac     "$PLAYER_MAC_DIR"
  report_player linux   "$PLAYER_LINUX_DIR"
  report_player windows "$PLAYER_WINDOWS_DIR"
}

stage_example() {
  local src="$1" stage="$2"
  cleanup_stage "$stage"
  mkdir -p "$stage/App" "$stage/Editor" || return 1
  cp -a "$src/." "$stage/App/" || return 1
  uav_log "staged example  : $stage/App"
  write_player_builder "$stage/Editor/UnitedAVPlayerBuilder.cs"
}

cleanup_stage() {
  local stage="$1"
  [ -n "$stage" ] || return 0
  if [ -d "$stage" ]; then
    rm -rf "$stage" "$stage.meta" 2>/dev/null || true
    uav_log "cleaned staged  : $stage"
  fi
}

write_player_builder() {
  local out="$1"
  local scene_in_project="$STAGE_REL/App/EndToEnd.unity"
  cat > "$out" <<EOF
// GENERATED by tests/unity/build-on-mac.sh — transient; removed after the build.
// Cross-builds the EndToEnd example app into a standalone Player per BuildTarget.
using System;
using System.IO;
using UnityEditor;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace UnitedAV.Build
{
    public static class PlayerBuilder
    {
        const string Scene = "$scene_in_project";

        public static void BuildAll()
        {
            string targetsEnv = Environment.GetEnvironmentVariable("UAV_BUILD_TARGETS");
            string outputRoot = Environment.GetEnvironmentVariable("UAV_BUILD_OUTPUT");
            string playerName = Environment.GetEnvironmentVariable("UAV_PLAYER_NAME");
            if (string.IsNullOrEmpty(targetsEnv)) targetsEnv = "StandaloneOSX StandaloneWindows64 StandaloneLinux64";
            if (string.IsNullOrEmpty(outputRoot)) outputRoot = Path.Combine(Directory.GetCurrentDirectory(), "Players");
            if (string.IsNullOrEmpty(playerName)) playerName = "$PLAYER_NAME";

            if (!File.Exists(Scene))
                Debug.LogError("[uav-build] staged scene not found: " + Scene);

            int failures = 0;
            foreach (var tok in targetsEnv.Split(new[] { ' ', ',' }, StringSplitOptions.RemoveEmptyEntries))
            {
                BuildTarget bt;
                try { bt = (BuildTarget)Enum.Parse(typeof(BuildTarget), tok, true); }
                catch { Debug.LogError("[uav-build] unknown BuildTarget: " + tok); failures++; continue; }

                if (!BuildPipeline.IsBuildTargetSupported(BuildTargetGroup.Standalone, bt))
                {
                    Debug.LogWarning("[uav-build] SKIP " + bt + ": Standalone Build Support not installed.");
                    continue;
                }

                string sub = SubdirFor(bt);
                string dir = Path.Combine(outputRoot, sub);
                Directory.CreateDirectory(dir);
                string location = Path.Combine(dir, playerName + ExtFor(bt));

                var opts = new BuildPlayerOptions
                {
                    scenes = new[] { Scene },
                    locationPathName = location,
                    target = bt,
                    targetGroup = BuildTargetGroup.Standalone,
                    options = BuildOptions.None,
                };
                Debug.Log("[uav-build] building " + bt + " -> " + location);
                var report = BuildPipeline.BuildPlayer(opts);
                var sum = report.summary;
                if (sum.result == BuildResult.Succeeded)
                    Debug.Log("[uav-build] OK " + bt + " size=" + sum.totalSize + " out=" + sum.outputPath);
                else
                {
                    Debug.LogError("[uav-build] FAIL " + bt + " result=" + sum.result + " errors=" + sum.totalErrors);
                    failures++;
                }
            }

            if (failures > 0)
                EditorApplication.Exit(1);   // non-zero so the shell records a blocker
        }

        static string SubdirFor(BuildTarget bt)
        {
            switch (bt)
            {
                case BuildTarget.StandaloneOSX:        return "mac";
                case BuildTarget.StandaloneWindows64:
                case BuildTarget.StandaloneWindows:    return "windows";
                case BuildTarget.StandaloneLinux64:    return "linux";
                default:                               return bt.ToString();
            }
        }

        static string ExtFor(BuildTarget bt)
        {
            switch (bt)
            {
                case BuildTarget.StandaloneOSX:        return ".app";
                case BuildTarget.StandaloneWindows64:
                case BuildTarget.StandaloneWindows:    return ".exe";
                case BuildTarget.StandaloneLinux64:    return ".x86_64";
                default:                               return "";
            }
        }
    }
}
EOF
  uav_log "generated build : $out"
}

report_player() {
  local target="$1" dir="$2" found=""
  case "$target" in
    mac)     found="$(/usr/bin/find "$dir" -maxdepth 2 -name '*.app' 2>/dev/null | head -1)"
             [ -z "$found" ] && found="$(find "$dir" -maxdepth 2 -name '*.app' 2>/dev/null | head -1)" ;;
    windows) found="$(find "$dir" -maxdepth 2 -name '*.exe' 2>/dev/null | head -1)" ;;
    linux)   found="$(find "$dir" -maxdepth 2 -type f \( -name "$PLAYER_NAME" -o -name '*.x86_64' \) 2>/dev/null | head -1)" ;;
  esac
  if [ -n "$found" ]; then
    uav_log "player $target  : BUILT -> $found"
  else
    uav_log "player $target  : NOT FOUND under $dir"
    note_blocker "$target Player artifact not produced (module missing or build error). The run-$target.sh leg will skip."
  fi
}

case "$ONLY" in
  modules) install_modules ;;
  tests)   gather_plugins; run_tests || true ;;
  build)   gather_plugins; build_players ;;
  all)
    install_modules
    gather_plugins
    run_tests || true
    build_players
    ;;
  *) uav_die "unknown UAV_ONLY=$ONLY (use all|modules|tests|build)" ;;
esac

uav_section "Build-hub summary"
if [ "${#BLOCKERS[@]}" -eq 0 ]; then
  uav_log "no blockers recorded."
else
  uav_log "${#BLOCKERS[@]} blocker(s):"
  for b in "${BLOCKERS[@]}"; do printf '  - %s\n' "$b"; done
fi
uav_log "Players (for run-*.sh): $PLAYERS_DIR"
uav_log "Next: run each Player on its target —"
uav_log "  mac     : bash tests/unity/run-mac.sh         (on macOS)"
uav_log "  linux   : bash tests/unity/run-linux.sh       (locally, xvfb)"
uav_log "  windows : pwsh tests/unity/run-windows.ps1    (in a Windows VM)"
exit 0
