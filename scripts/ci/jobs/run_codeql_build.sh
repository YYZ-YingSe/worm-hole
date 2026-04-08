#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$ROOT/scripts/ci/common.sh"

if ! wh_has_cmake_project; then
  echo "[codeql-build] SKIP no CMakeLists.txt"
  exit 0
fi

if ! wh_has_command cmake; then
  echo "[codeql-build] FAIL cmake required"
  exit 1
fi

build_dir="$(wh_ci_build_dir "codeql")"

cmake_args=(
  -DWH_BUILD_TESTING=ON
  -DWH_BUILD_EXAMPLES=OFF
  -DWH_BUILD_BENCHMARKS=OFF
  -DWH_WARNINGS_AS_ERRORS=OFF
)

wh_ci_append_standard_third_party_args cmake_args
wh_ci_configure_build_dir "$build_dir" "${cmake_args[@]}"
wh_build_test_artifacts "$build_dir" ""

echo "[codeql-build] PASS"
