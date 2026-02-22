#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

strict_mode="${WH_CI_STRICT:-0}"

if [[ ! -f CMakeLists.txt ]]; then
  if [[ "$strict_mode" == "1" || -n "${CI:-}" ]]; then
    echo "[scan-build] SKIP no CMakeLists.txt"
    exit 0
  fi
  echo "[scan-build] SKIP no CMakeLists.txt"
  exit 0
fi

if ! command -v scan-build >/dev/null 2>&1; then
  if [[ "$strict_mode" == "1" || -n "${CI:-}" ]]; then
    echo "[scan-build] FAIL scan-build not installed in strict mode"
    exit 1
  fi
  echo "[scan-build] SKIP scan-build not installed"
  exit 0
fi

build_dir="build/ci-scan-build"
scan-build --status-bugs cmake -S . -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DWH_BUILD_TESTING=OFF \
  -DWH_WARNINGS_AS_ERRORS=ON
scan-build --status-bugs cmake --build "$build_dir"

echo "[scan-build] PASS"
