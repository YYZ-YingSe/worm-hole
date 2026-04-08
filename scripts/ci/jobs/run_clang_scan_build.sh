#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$ROOT/scripts/ci/common.sh"

if [[ "${RUNNER_OS:-}" == "Windows" ]]; then
  echo "[scan-build] SKIP windows runner (scan-build toolchain integration not stable)"
  exit 0
fi

if ! wh_ci_require_cmake_project "scan-build"; then
  exit 0
fi

if ! wh_ci_require_commands "scan-build" scan-build cmake; then
  exit 0
fi

build_dir="$(wh_ci_build_dir "scan-build")"
cmake_args=(
  -DCMAKE_BUILD_TYPE=Debug
  -DCMAKE_CXX_COMPILER=clang++
  -DWH_BUILD_TESTING=ON
  -DWH_BUILD_EXAMPLES=OFF
  -DWH_BUILD_BENCHMARKS=OFF
  -DWH_WARNINGS_AS_ERRORS=ON
)
wh_ci_append_standard_third_party_args cmake_args

scan-build --status-bugs cmake -S . -B "$build_dir" -G "$(wh_default_cmake_generator)" "${cmake_args[@]}"
wh_sync_compile_commands "$build_dir"
scan-build --status-bugs cmake --build "$build_dir" --target "$(wh_target_test_artifacts)"

echo "[scan-build] PASS"
