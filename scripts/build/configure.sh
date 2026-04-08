#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/build/common.sh"
wh_enter_build_root

build_type="${WH_BUILD_TYPE:-Debug}"
build_dir="${WH_BUILD_DIR:-$(wh_default_manual_env_build_dir)}"
generator="${WH_CMAKE_GENERATOR:-$(wh_default_cmake_generator)}"

thirdy_party_dir="${WH_THIRDY_PARTY_DIR:-${ROOT}/thirdy_party}"
strict_thirdy_party="${WH_REQUIRE_GIT_LOCKED_THIRDY_PARTY:-ON}"

if [[ "${WH_SYNC_THIRDY_PARTY:-0}" == "1" ]]; then
  git submodule sync --recursive
  git submodule update --init --recursive
fi

cmake_args=(
  -DCMAKE_BUILD_TYPE="$build_type"
  -DWH_BUILD_TESTING="${WH_BUILD_TESTING:-ON}"
  -DWH_BUILD_UT="${WH_BUILD_UT:-ON}"
  -DWH_BUILD_FT="${WH_BUILD_FT:-ON}"
  -DWH_BUILD_EXAMPLES="${WH_BUILD_EXAMPLES:-OFF}"
  -DWH_BUILD_BENCHMARKS="${WH_BUILD_BENCHMARKS:-OFF}"
  -DWH_REQUIRE_GIT_LOCKED_THIRDY_PARTY="$strict_thirdy_party"
  -DWH_THIRDY_PARTY_DIR="$thirdy_party_dir"
  -DWH_WARNINGS_AS_ERRORS="${WH_WARNINGS_AS_ERRORS:-ON}"
  -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
)

if wh_has_command ccache; then
  cmake_args+=("$(wh_enable_ccache "configure")")
fi

wh_cmake_configure "$build_dir" "$generator" "${cmake_args[@]}"
wh_sync_compile_commands "$build_dir"
wh_print_ccache_stats

echo "[configure] PASS $build_dir"
