#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

strict_mode="${WH_CI_STRICT:-0}"
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

if [[ ! -f CMakeLists.txt ]]; then
  echo "[sanitizer-smoke] SKIP no CMakeLists.txt"
  exit 0
fi

if ! command -v cmake >/dev/null 2>&1 || ! command -v ctest >/dev/null 2>&1; then
  if [[ "$strict_mode" == "1" || -n "${CI:-}" ]]; then
    echo "[sanitizer-smoke] FAIL cmake/ctest required"
    exit 1
  fi
  echo "[sanitizer-smoke] SKIP cmake/ctest not installed"
  exit 0
fi

build_dir="build/ci-sanitizer-smoke"
san_flags='-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g'

cmake_args=(
  -DCMAKE_BUILD_TYPE=Debug
  -DWH_BUILD_TESTING=ON
  -DWH_REQUIRE_GIT_LOCKED_THIRDY_PARTY=ON
  -DWH_THIRDY_PARTY_DIR="${WH_THIRDY_PARTY_DIR:-${ROOT}/thirdy_party}"
  -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
  -DCMAKE_CXX_FLAGS="$san_flags"
)

if command -v ccache >/dev/null 2>&1; then
  echo "[sanitizer-smoke] ccache enabled"
  ccache -z >/dev/null 2>&1 || true
  cmake_args+=( -DCMAKE_CXX_COMPILER_LAUNCHER=ccache )
fi

cmake -S . -B "$build_dir" -G Ninja "${cmake_args[@]}"
cmake --build "$build_dir" --parallel

if command -v ccache >/dev/null 2>&1; then
  ccache -s || true
fi

if ctest --test-dir "$build_dir" -N 2>/dev/null | rg -q 'Total Tests:[[:space:]]*[1-9]'; then
  ASAN_OPTIONS="$asan_options_effective" UBSAN_OPTIONS="$ubsan_options_effective" \
    ctest --test-dir "$build_dir" --output-on-failure --timeout 120
  echo "[sanitizer-smoke] PASS"
else
  if [[ "$strict_mode" == "1" || -n "${CI:-}" ]]; then
    echo "[sanitizer-smoke] FAIL no tests discovered in strict mode"
    exit 1
  fi
  echo "[sanitizer-smoke] SKIP no tests"
fi
