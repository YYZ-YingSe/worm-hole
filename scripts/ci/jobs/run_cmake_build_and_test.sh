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
test_shard_count="${WH_TEST_SHARD_COUNT:-1}"
test_shard_index="${WH_TEST_SHARD_INDEX:-0}"
test_suite_filter="${WH_TEST_SUITE_FILTER:-}"

if ! wh_ci_require_cmake_project "build-verify"; then
  exit 0
fi

if ! wh_ci_require_commands "build-verify" cmake ctest python3; then
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
manifest_path="${build_dir%/}/wh_test_manifest.tsv"
shard_args=()

if [[ "$enable_tests" == "1" && "$test_shard_count" != "1" ]]; then
  shard_args=(
    --manifest "$manifest_path"
    --shard-count "$test_shard_count"
    --shard-index "$test_shard_index"
  )

  if [[ -n "$test_suite_filter" ]]; then
    OLDIFS="$IFS"
    IFS=',' read -r -a test_suites <<< "$test_suite_filter"
    IFS="$OLDIFS"
    suite_name=""
    for suite_name in "${test_suites[@]}"; do
      [[ -n "$suite_name" ]] || continue
      shard_args+=(--suite "$suite_name")
    done
  fi

  shard_targets=()
  while IFS= read -r target_name; do
    [[ -n "$target_name" ]] || continue
    shard_targets+=("$target_name")
  done < <(
    python3 "$ROOT/scripts/testing/shard_test_manifest.py" \
      "${shard_args[@]}" \
      --emit targets)

  if [[ "${#shard_targets[@]}" -eq 0 ]]; then
    wh_ci_fail "build-verify" "empty test shard target selection"
    exit 1
  fi

  echo "[build-verify] shard ${test_shard_index}/${test_shard_count} targets: ${#shard_targets[@]}"
  wh_cmake_build_targets "$build_dir" "" "${shard_targets[@]}"
else
  wh_build_enabled_artifacts "$build_dir" ""
fi

wh_print_ccache_stats

if [[ "$enable_tests" != "1" ]]; then
  echo "[build-verify] PASS build only (tests disabled by WH_CI_ENABLE_TESTS) ($build_type)"
  exit 0
fi

if [[ "$test_shard_count" != "1" ]]; then
  python3 "$ROOT/scripts/testing/shard_test_manifest.py" \
    "${shard_args[@]}" \
    --run \
    --reporter compact \
    --timeout-seconds 120
  echo "[build-verify] PASS sharded build+tests ($build_type)"
elif wh_ci_run_ctest_or_skip "build-verify" "$build_dir"; then
  echo "[build-verify] PASS build+tests ($build_type)"
else
  echo "[build-verify] PASS build only, no tests ($build_type)"
fi
