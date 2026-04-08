#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$ROOT/scripts/ci/common.sh"

enable_tests="${WH_CI_ENABLE_TESTS:-1}"
enable_examples="${WH_CI_ENABLE_EXAMPLES:-0}"
enable_benchmarks="${WH_CI_ENABLE_BENCHMARKS:-0}"
build_type="${WH_CI_BUILD_TYPE:-Debug}"
build_type_lc="$(wh_lowercase "$build_type")"
build_dir="$(wh_ci_build_dir "$build_type_lc")"

if ! wh_ci_require_cmake_project "build-verify"; then
  exit 0
fi

if ! wh_ci_require_commands "build-verify" cmake ctest; then
  exit 0
fi

cxx_compiler="${CXX:-clang++}"
warnings_as_errors="ON"
if [[ "${WH_CI_WERROR:-1}" != "1" ]]; then
  warnings_as_errors="OFF"
fi

build_testing_flag="ON"
if [[ "$enable_tests" != "1" ]]; then
  build_testing_flag="OFF"
fi

build_examples_flag="ON"
if [[ "$enable_examples" != "1" ]]; then
  build_examples_flag="OFF"
fi

build_benchmarks_flag="ON"
if [[ "$enable_benchmarks" != "1" ]]; then
  build_benchmarks_flag="OFF"
fi

cmake_args=(
  -DCMAKE_BUILD_TYPE="$build_type"
  -DWH_BUILD_TESTING="$build_testing_flag"
  -DWH_BUILD_EXAMPLES="$build_examples_flag"
  -DWH_BUILD_BENCHMARKS="$build_benchmarks_flag"
  -DWH_WARNINGS_AS_ERRORS="$warnings_as_errors"
  -DCMAKE_CXX_COMPILER="$cxx_compiler"
)

wh_ci_append_standard_third_party_args cmake_args
wh_ci_maybe_enable_ccache cmake_args "build-verify"

wh_ci_configure_build_dir "$build_dir" "${cmake_args[@]}"
wh_build_enabled_artifacts "$build_dir" ""

wh_print_ccache_stats

if [[ "$enable_tests" != "1" ]]; then
  echo "[build-verify] PASS build only (tests disabled by WH_CI_ENABLE_TESTS) ($build_type)"
  exit 0
fi

if wh_ci_run_ctest_or_skip "build-verify" "$build_dir"; then
  echo "[build-verify] PASS build+tests ($build_type)"
else
  echo "[build-verify] PASS build only, no tests ($build_type)"
fi
