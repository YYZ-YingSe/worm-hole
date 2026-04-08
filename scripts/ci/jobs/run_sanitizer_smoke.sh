#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$ROOT/scripts/ci/common.sh"

lsan_mode="${WH_LSAN_MODE:-auto}"

asan_options_default="detect_leaks=1"
lsan_reason="enabled"

probe_lsan_runtime() {
  local os_name
  os_name="$(uname -s)"

  if [[ "$os_name" == "Darwin" ]]; then
    lsan_reason="disabled on darwin sandboxed runners"
    return 1
  fi

  if ! command -v ps >/dev/null 2>&1; then
    lsan_reason="disabled: ps command not found"
    return 1
  fi

  if ! /bin/ps -p $$ -o pid= >/dev/null 2>&1; then
    lsan_reason="disabled: ps exists but execution is not permitted"
    return 1
  fi

  lsan_reason="enabled"
  return 0
}

case "$lsan_mode" in
  on)
    asan_options_default="detect_leaks=1"
    lsan_reason="forced on by WH_LSAN_MODE=on"
    ;;
  off)
    asan_options_default="detect_leaks=0"
    lsan_reason="forced off by WH_LSAN_MODE=off"
    ;;
  auto)
    if ! probe_lsan_runtime; then
      asan_options_default="detect_leaks=0"
    fi
    ;;
  *)
    echo "[sanitizer-smoke] FAIL invalid WH_LSAN_MODE=$lsan_mode (expected: auto|on|off)"
    exit 1
    ;;
esac

asan_options_effective="${ASAN_OPTIONS:-$asan_options_default}"
ubsan_options_effective="${UBSAN_OPTIONS:-print_stacktrace=1}"

echo "[sanitizer-smoke] lsan_mode=$lsan_mode ($lsan_reason)"
echo "[sanitizer-smoke] ASAN_OPTIONS=$asan_options_effective"

if ! wh_ci_require_cmake_project "sanitizer-smoke"; then
  exit 0
fi

if ! wh_ci_require_commands "sanitizer-smoke" cmake ctest; then
  exit 0
fi

build_dir="$(wh_ci_build_dir "sanitizer-smoke")"
san_flags='-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g'

cmake_args=(
  -DCMAKE_BUILD_TYPE=Debug
  -DWH_BUILD_TESTING=ON
  -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
  -DCMAKE_CXX_FLAGS="$san_flags"
)

wh_ci_append_standard_third_party_args cmake_args
wh_ci_maybe_enable_ccache cmake_args "sanitizer-smoke"

wh_ci_configure_build_dir "$build_dir" "${cmake_args[@]}"
wh_build_test_artifacts "$build_dir" ""

wh_print_ccache_stats

if wh_ctest_has_tests "$build_dir"; then
  ASAN_OPTIONS="$asan_options_effective" UBSAN_OPTIONS="$ubsan_options_effective" \
    ctest --test-dir "$build_dir" --output-on-failure --timeout 120
  echo "[sanitizer-smoke] PASS"
else
  if wh_is_strict_mode; then
    echo "[sanitizer-smoke] FAIL no tests discovered in strict mode"
    exit 1
  fi
  echo "[sanitizer-smoke] SKIP no tests"
fi
