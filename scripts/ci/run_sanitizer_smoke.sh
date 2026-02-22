#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

strict_mode="${WH_CI_STRICT:-0}"

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

cmake -S . -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER="${CC:-clang}" \
  -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
  -DCMAKE_C_FLAGS="$san_flags" \
  -DCMAKE_CXX_FLAGS="$san_flags"

cmake --build "$build_dir" --parallel

if ctest --test-dir "$build_dir" -N 2>/dev/null | rg -q 'Total Tests:[[:space:]]*[1-9]'; then
  ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
    ctest --test-dir "$build_dir" --output-on-failure --timeout 120
  echo "[sanitizer-smoke] PASS"
else
  echo "[sanitizer-smoke] SKIP no tests"
fi
