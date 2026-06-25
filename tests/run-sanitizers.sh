#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WHICH="${UAV_SAN:-both}"

case "$WHICH" in
  address) CONFIGS=(address) ;;
  thread)  CONFIGS=(thread) ;;
  both)    CONFIGS=(address thread) ;;
  *) echo "UAV_SAN must be 'address', 'thread' or 'both' (got '$WHICH')" >&2; exit 2 ;;
esac

command -v cmake >/dev/null || { echo "cmake not on PATH (run inside the dev shell)" >&2; exit 1; }

declare -a SUMMARY=()
OVERALL_RC=0

run_one() {
  local san="$1" build_dir
  case "$san" in
    address) build_dir="$REPO_DIR/native/build-asan" ;;
    thread)  build_dir="$REPO_DIR/native/build-tsan" ;;
  esac

  echo
  echo "=========================================================================="
  echo "=== UnitedAV Tier-1 sanitizer gate: UAV_SANITIZE=$san  build=$build_dir"
  echo "=========================================================================="

  if ! cmake -S "$REPO_DIR/native" -B "$build_dir" -G Ninja \
        -DUAV_SANITIZE="$san" -DUAV_BUILD_TESTS=ON \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo; then
    echo "FATAL: cmake configure failed for $san" >&2
    SUMMARY+=("$san|CONFIGURE-FAILED|$build_dir"); OVERALL_RC=1; return 1
  fi

  if ! cmake --build "$build_dir"; then
    echo "FATAL: build failed for $san" >&2
    SUMMARY+=("$san|BUILD-FAILED|$build_dir"); OVERALL_RC=1; return 1
  fi

  local tests_list
  tests_list="$(ctest --test-dir "$build_dir" -N 2>/dev/null || true)"
  echo "$tests_list"
  if ! grep -q 'uav_tests' <<<"$tests_list"; then
    echo "FATAL: uav_tests not discovered in $build_dir — the sanitizer build did" >&2
    echo "       not pick up native/tests/unit/*.cpp (stale configure?). Aborting." >&2
    SUMMARY+=("$san|NO-SUITES|$build_dir"); OVERALL_RC=1; return 1
  fi
  if ! grep -q 'oracle_selftest' <<<"$tests_list"; then
    echo "WARN: oracle_selftest not discovered (FFmpeg dev libs absent?). Unit" >&2
    echo "      suite still gets its sanitizer pass; oracle is skipped here." >&2
  fi

  if [ -f "$build_dir/build.ninja" ]; then
    local pat
    if [ "$san" = address ]; then pat='fsanitize=address'; else pat='fsanitize=thread'; fi
    if ! grep -q -- "$pat" "$build_dir/build.ninja"; then
      echo "FATAL: no -$pat in $build_dir/build.ninja — not instrumented." >&2
      SUMMARY+=("$san|NOT-INSTRUMENTED|$build_dir"); OVERALL_RC=1; return 1
    fi
  fi

  if [ "$san" = address ]; then
    export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:strict_string_checks=1"
    export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
  else
    export TSAN_OPTIONS="suppressions=$REPO_DIR/native/tools/tsan.supp:halt_on_error=1"
  fi

  # shellcheck disable=SC2086
  if ctest --test-dir "$build_dir" -L tier1 --output-on-failure ${CTEST_ARGS:-}; then
    echo "=== sanitizer gate ($san) PASSED: Tier-1 suites ran instrumented ==="
    SUMMARY+=("$san|PASS|$build_dir")
  else
    echo "=== sanitizer gate ($san) FAILED ===" >&2
    SUMMARY+=("$san|FAIL|$build_dir"); OVERALL_RC=1; return 1
  fi
}

for san in "${CONFIGS[@]}"; do
  run_one "$san" || true
done

echo
echo "=========================================================================="
echo "  UnitedAV Tier-1 sanitizer gate — SUMMARY"
echo "=========================================================================="
printf '%-9s %-18s %s\n' "CONFIG" "RESULT" "BUILD DIR"
echo "--------------------------------------------------------------------------"
for row in "${SUMMARY[@]}"; do
  IFS='|' read -r cfg res dir <<<"$row"
  printf '%-9s %-18s %s\n' "$cfg" "$res" "$dir"
done
echo "--------------------------------------------------------------------------"
if [ "$OVERALL_RC" -eq 0 ]; then
  echo "OVERALL: PASS"
else
  echo "OVERALL: FAIL"
fi
exit "$OVERALL_RC"
