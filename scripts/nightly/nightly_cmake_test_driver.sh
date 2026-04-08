#!/usr/bin/env bash
set -euo pipefail

# Public local entrypoint for nightly sanitizer and labeled-test suites.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/ci/common.sh"

usage() {
  echo "usage: $0 sanitizer <asan|ubsan|tsan> | label <linearizability|stress|fuzz>"
}

configure_and_build() {
  local dir="$1"
  shift

  local cmake_args=(-DCMAKE_BUILD_TYPE=Debug)
  wh_ci_append_standard_third_party_args cmake_args
  local arg=""
  for arg in "$@"; do
    cmake_args+=("$arg")
  done

  wh_ci_configure_build_dir "$dir" "${cmake_args[@]}"
  wh_build_test_artifacts "$dir" ""
}

if [[ $# -ne 2 ]]; then
  usage
  exit 2
fi

mode="$1"
value="$2"

if ! wh_ci_require_cmake_project "nightly"; then
  exit 0
fi

wh_ci_require_commands_or_fail "nightly" cmake ctest

case "$mode" in
  sanitizer)
    case "$value" in
      asan|ubsan|tsan) ;;
      *) usage; exit 2 ;;
    esac

    build_dir="$(wh_nightly_build_dir "sanitizer-${value}")"
    san_flag="-fsanitize=${value} -fno-omit-frame-pointer"
    configure_and_build "$build_dir" \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_CXX_FLAGS="$san_flag"

    if wh_ci_run_ctest_or_skip "nightly-${value}" "$build_dir"; then
      echo "[nightly-${value}] PASS"
    fi
    ;;

  label)
    build_dir="$(wh_nightly_build_dir "label-${value}")"
    configure_and_build "$build_dir" \
      -DCMAKE_CXX_COMPILER=clang++

    if wh_ctest_has_label_tests "$build_dir" "$value"; then
      ctest --test-dir "$build_dir" --output-on-failure -L "$value"
      echo "[nightly-${value}] PASS"
    else
      echo "[nightly-${value}] SKIP no labeled tests"
    fi
    ;;

  *)
    usage
    exit 2
    ;;
esac
