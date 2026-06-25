#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WHICH="${UAV_SAN:-both}"
DO_FUZZ="${UAV_SANGATE_FUZZ:-1}"
DO_ORACLE="${UAV_SANGATE_ORACLE:-1}"
SUPP="$REPO_DIR/native/tools/tsan.supp"

case "$WHICH" in
  address) CONFIGS=(address) ;;
  thread)  CONFIGS=(thread) ;;
  both)    CONFIGS=(address thread) ;;
  *) echo "UAV_SAN must be 'address', 'thread' or 'both' (got '$WHICH')" >&2; exit 2 ;;
esac

command -v cmake >/dev/null || { echo "cmake not on PATH (run inside the dev shell)" >&2; exit 1; }
command -v ctest >/dev/null || { echo "ctest not on PATH (run inside the dev shell)" >&2; exit 1; }

declare -a SUMMARY=()
OVERALL_RC=0

note_fail() { OVERALL_RC=1; }

check_supp_ffmpeg_only() {
  if [ ! -f "$SUPP" ]; then
    echo "FATAL: TSan suppressions file missing: $SUPP" >&2
    SUMMARY+=("supp-check|MISSING-SUPP|$SUPP"); note_fail; return 1
  fi
  local offending
  offending="$(grep -E '^[[:space:]]*(race|thread):' "$SUPP" \
                | grep -iE 'uav|UnitedAV' || true)"
  if [ -n "$offending" ]; then
    echo "FATAL: native/tools/tsan.supp contains a NON-FFmpeg (UnitedAV) suppression:" >&2
    echo "$offending" | sed 's/^/    /' >&2
    echo "       tsan.supp must remain FFmpeg-ONLY (see this script's header)." >&2
    SUMMARY+=("supp-check|UAV-SUPPRESSION|$SUPP"); note_fail; return 1
  fi
  local bad
  bad="$(grep -E '^[[:space:]]*(race|thread):' "$SUPP" \
          | grep -vE ':[[:space:]]*(ff_|av_|frame_worker_thread)' || true)"
  if [ -n "$bad" ]; then
    echo "WARN: tsan.supp has entries not matching ff_*/av_*/frame_worker_thread:" >&2
    echo "$bad" | sed 's/^/    /' >&2
    echo "      (not failing — but confirm these are FFmpeg-internal symbols.)" >&2
  fi
  echo "GATE: tsan.supp is FFmpeg-only (no uav::/UnitedAV suppression) — OK"
  SUMMARY+=("supp-check|PASS|$SUPP")
  return 0
}

configure_build_selfcheck() {
  local san="$1" build_dir="$2" extra_cmake="${3:-}"

  echo
  echo "=========================================================================="
  echo "=== sangate: configure+build  UAV_SANITIZE=$san  build=$build_dir"
  echo "=========================================================================="

  # shellcheck disable=SC2086
  if ! cmake -S "$REPO_DIR/native" -B "$build_dir" -G Ninja \
        -DUAV_SANITIZE="$san" -DUAV_BUILD_TESTS=ON \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo $extra_cmake; then
    echo "FATAL: cmake configure failed for $san ($build_dir)" >&2
    return 10
  fi

  if ! cmake --build "$build_dir"; then
    echo "FATAL: build failed for $san ($build_dir)" >&2
    return 11
  fi

  local tests_list
  tests_list="$(ctest --test-dir "$build_dir" -N 2>/dev/null || true)"
  echo "$tests_list"
  if ! grep -q 'uav_tests' <<<"$tests_list"; then
    echo "FATAL: uav_tests not discovered in $build_dir (stale configure / GLOB miss)." >&2
    return 12
  fi
  if [ -f "$build_dir/build.ninja" ]; then
    local pat
    if [ "$san" = address ]; then pat='fsanitize=address'; else pat='fsanitize=thread'; fi
    if ! grep -q -- "$pat" "$build_dir/build.ninja"; then
      echo "FATAL: no -$pat in $build_dir/build.ninja — not instrumented." >&2
      return 13
    fi
  fi
  if grep -q 'oracle_selftest' <<<"$tests_list"; then
    UAV_ORACLE_PRESENT=1
  else
    UAV_ORACLE_PRESENT=0
    echo "WARN: uav_oracle not discovered (FFmpeg dev libs absent?). The decode" >&2
    echo "      matrix leg under ASan will be SKIPPED (not silently passed)." >&2
  fi
  return 0
}

run_tier12() {
  local san="$1" build_dir="$2"
  if [ "$san" = address ]; then
    export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:strict_string_checks=1"
    export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
    unset TSAN_OPTIONS
  else
    export TSAN_OPTIONS="suppressions=$SUPP:halt_on_error=1"
    unset ASAN_OPTIONS UBSAN_OPTIONS
  fi
  # shellcheck disable=SC2086
  ctest --test-dir "$build_dir" -L 'tier1|tier2' --output-on-failure ${CTEST_ARGS:-}
}

run_concurrency_isolated_tsan() {
  local build_dir="$1"
  local bin="$build_dir/uav_tests"
  if [ ! -x "$bin" ]; then
    echo "FATAL: uav_tests binary not found at $bin for the isolated concurrency leg." >&2
    return 1
  fi
  export TSAN_OPTIONS="suppressions=$SUPP:halt_on_error=1"
  unset ASAN_OPTIONS UBSAN_OPTIONS

  local listed
  listed="$("$bin" --test-suite='*concurrency*' --list-test-suites 2>/dev/null || true)"
  if ! grep -qiE 'concurrency' <<<"$listed"; then
    echo "FATAL: --test-suite='*concurrency*' matched NO test cases in $bin." >&2
    echo "       The [concurrency] race probes are missing or were renamed — the" >&2
    echo "       isolated TSan leg would be VACUOUS. Refusing to report a pass." >&2
    return 1
  fi
  echo "concurrency (isolated TSan): running the [concurrency] race probes under TSan"
  echo "  (filter: --test-suite='*concurrency*', dedicated 600s timeout)"
  timeout 600 "$bin" --test-suite='*concurrency*'
}

run_oracle_matrix_asan() {
  local build_dir="$1"
  if [ "$DO_ORACLE" != 1 ]; then
    echo "oracle matrix: DISABLED (UAV_SANGATE_ORACLE=0) — skipping"
    return 0
  fi
  if [ "${UAV_ORACLE_PRESENT:-0}" != 1 ]; then
    echo "oracle matrix: uav_oracle absent (no FFmpeg) — skipping"
    return 0
  fi
  local n
  n="$(ctest --test-dir "$build_dir" -N -L oracle 2>/dev/null \
        | grep -cE 'Test +#' || true)"
  if [ "${n:-0}" -le 1 ]; then
    echo "oracle matrix: no per-clip clips in tests/media/out — skipping (run tests/media/gen.sh)"
    return 0
  fi
  export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:strict_string_checks=1"
  export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
  unset TSAN_OPTIONS
  echo "oracle matrix: running $n oracle case(s) under ASan/UBSan/LSan"
  ctest --test-dir "$build_dir" -L oracle --output-on-failure
}

run_config() {
  local san="$1" build_dir
  case "$san" in
    address) build_dir="$REPO_DIR/native/build-asan" ;;
    thread)  build_dir="$REPO_DIR/native/build-tsan" ;;
  esac

  if ! configure_build_selfcheck "$san" "$build_dir"; then
    SUMMARY+=("$san|CONFIGURE/BUILD/SELFCHECK-FAIL|$build_dir"); note_fail; return
  fi

  if run_tier12 "$san" "$build_dir"; then
    if [ "$san" = address ]; then
      SUMMARY+=("asan (unit+misuse+netfault)|PASS|$build_dir")
    else
      SUMMARY+=("tsan (full suite)|PASS|$build_dir")
    fi
  else
    if [ "$san" = address ]; then
      SUMMARY+=("asan (unit+misuse+netfault)|FAIL|$build_dir")
    else
      SUMMARY+=("tsan (full suite)|FAIL|$build_dir")
    fi
    note_fail
  fi

  if [ "$san" = thread ]; then
    if run_concurrency_isolated_tsan "$build_dir"; then
      SUMMARY+=("tsan-concurrency (isolated)|PASS|$build_dir")
    else
      SUMMARY+=("tsan-concurrency (isolated)|FAIL|$build_dir"); note_fail
    fi
  fi

  if [ "$san" = address ]; then
    if run_oracle_matrix_asan "$build_dir"; then
      SUMMARY+=("asan-oracle (decode matrix)|PASS-or-SKIP|$build_dir")
    else
      SUMMARY+=("asan-oracle (decode matrix)|FAIL|$build_dir"); note_fail
    fi
  fi
}

run_fuzz_smoke() {
  if [ "$DO_FUZZ" != 1 ]; then
    echo "fuzz smoke: DISABLED (UAV_SANGATE_FUZZ=0) — skipping"
    SUMMARY+=("fuzz-smoke|SKIP(disabled)|-")
    return 0
  fi
  local cxx="${CXX:-}"
  local is_clang=0
  if [ -n "$cxx" ] && "$cxx" --version 2>/dev/null | grep -qi clang; then
    is_clang=1
  elif command -v clang++ >/dev/null 2>&1; then
    cxx="$(command -v clang++)"; is_clang=1
  fi
  if [ "$is_clang" != 1 ]; then
    echo "fuzz smoke: compiler is not clang (libFuzzer unavailable) — auto-skipping"
    SUMMARY+=("fuzz-smoke|SKIP(no-clang)|-")
    return 0
  fi

  local build_dir="$REPO_DIR/native/build-fuzz"
  echo
  echo "=========================================================================="
  echo "=== sangate: fuzz smoke (libFuzzer, clang)  build=$build_dir"
  echo "=========================================================================="
  if ! cmake -S "$REPO_DIR/native" -B "$build_dir" -G Ninja \
        -DUAV_BUILD_TESTS=ON -DUAV_FUZZ=ON \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER="$cxx" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo; then
    echo "FATAL: fuzz configure failed" >&2
    SUMMARY+=("fuzz-smoke|CONFIGURE-FAIL|$build_dir"); note_fail; return 1
  fi
  if ! cmake --build "$build_dir"; then
    echo "FATAL: fuzz build failed" >&2
    SUMMARY+=("fuzz-smoke|BUILD-FAIL|$build_dir"); note_fail; return 1
  fi

  local fuzz_list
  fuzz_list="$(ctest --test-dir "$build_dir" -N -L fuzz 2>/dev/null || true)"
  echo "$fuzz_list"
  if ! grep -qE 'fuzz_' <<<"$fuzz_list"; then
    echo "FATAL: no fuzz_* ctest cases discovered in $build_dir (GLOB/UAV_FUZZ miss)." >&2
    SUMMARY+=("fuzz-smoke|NO-FUZZ-TARGETS|$build_dir"); note_fail; return 1
  fi

  local have_ffmpeg=0
  if [ -f "$build_dir/build.ninja" ] \
     && grep -qE -- '(libav(format|codec|util|swscale|swresample)|-lavformat|-lavcodec|UAV_HAVE_FFMPEG=1)' \
                "$build_dir/build.ninja"; then
    have_ffmpeg=1
  elif [ -f "$build_dir/CMakeCache.txt" ] \
       && grep -qE '^FFMPEG_(FOUND:.*=1|LIBRARIES)' "$build_dir/CMakeCache.txt"; then
    have_ffmpeg=1
  fi
  if [ "$have_ffmpeg" != 1 ]; then
    echo "WARN: fuzz build does not link FFmpeg (libav* absent). The fuzz targets" >&2
    echo "      only exercise the create/open->UAV_ERR_UNSUPPORTED->teardown stub" >&2
    echo "      path — NONE of the demux/decode/encode attack surface they exist to" >&2
    echo "      cover is reached. Recording SKIP(no-ffmpeg), not an unqualified PASS" >&2
    echo "      (mirroring the oracle-absent SKIP)." >&2
    SUMMARY+=("fuzz-smoke|SKIP(no-ffmpeg)|$build_dir")
    return 0
  fi

  if ctest --test-dir "$build_dir" -L fuzz --output-on-failure; then
    echo "=== fuzz smoke PASSED ==="
    SUMMARY+=("fuzz-smoke|PASS|$build_dir")
  else
    echo "=== fuzz smoke FAILED ===" >&2
    SUMMARY+=("fuzz-smoke|FAIL|$build_dir"); note_fail
  fi
}

check_supp_ffmpeg_only || true

for san in "${CONFIGS[@]}"; do
  run_config "$san"
done

run_fuzz_smoke

echo
echo "=========================================================================="
echo "  UnitedAV Tier-2 sangate — SUMMARY"
echo "=========================================================================="
printf '%-32s %-18s %s\n' "CONFIG" "RESULT" "BUILD DIR"
echo "--------------------------------------------------------------------------"
for row in "${SUMMARY[@]}"; do
  IFS='|' read -r cfg res dir <<<"$row"
  printf '%-32s %-18s %s\n' "$cfg" "$res" "$dir"
done
echo "--------------------------------------------------------------------------"
if [ "$OVERALL_RC" -eq 0 ]; then
  echo "OVERALL: PASS"
else
  echo "OVERALL: FAIL"
fi
exit "$OVERALL_RC"
