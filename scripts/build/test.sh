#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/build/common.sh"
wh_enter_build_root

build_dir="${WH_BUILD_DIR:-$(wh_default_manual_env_build_dir)}"

bash scripts/build/build.sh

if wh_ctest_has_tests "$build_dir"; then
  ctest --test-dir "$build_dir" --output-on-failure --timeout 120
  echo "[test] PASS $build_dir"
else
  echo "[test] FAIL no tests discovered (check thirdy_party/catch2 and CMake test registration)"
  exit 1
fi
