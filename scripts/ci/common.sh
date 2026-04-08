#!/usr/bin/env bash

if [[ -n "${WH_CI_COMMON_SH_LOADED:-}" ]]; then
  return 0
fi
WH_CI_COMMON_SH_LOADED=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/build/common.sh"
wh_enter_build_root

wh_ci_fail() {
  local tag="$1"
  local message="$2"
  echo "[${tag}] FAIL ${message}"
}

wh_ci_skip() {
  local tag="$1"
  local message="$2"
  echo "[${tag}] SKIP ${message}"
}

wh_ci_require_cmake_project() {
  local tag="$1"
  if wh_has_cmake_project; then
    return 0
  fi

  wh_ci_skip "$tag" "no CMakeLists.txt"
  return 1
}

wh_ci_require_commands() {
  local tag="$1"
  shift

  local missing=()
  local cmd=""
  for cmd in "$@"; do
    if ! wh_has_command "$cmd"; then
      missing+=("$cmd")
    fi
  done

  if [[ "${#missing[@]}" -eq 0 ]]; then
    return 0
  fi

  if wh_is_strict_mode; then
    wh_ci_fail "$tag" "missing required tool: ${missing[*]}"
    exit 1
  fi

  wh_ci_skip "$tag" "missing required tool: ${missing[*]}"
  return 1
}

wh_ci_require_commands_or_fail() {
  local tag="$1"
  shift

  local missing=()
  local cmd=""
  for cmd in "$@"; do
    if ! wh_has_command "$cmd"; then
      missing+=("$cmd")
    fi
  done

  if [[ "${#missing[@]}" -eq 0 ]]; then
    return 0
  fi

  wh_ci_fail "$tag" "missing required tool: ${missing[*]}"
  exit 1
}

wh_ci_append_array_value() {
  local array_name="$1"
  local value="$2"
  local escaped=""
  printf -v escaped '%q' "$value"
  eval "${array_name}+=( ${escaped} )"
}

wh_ci_append_standard_third_party_args() {
  local array_name="$1"
  wh_ci_append_array_value "$array_name" "-DWH_REQUIRE_GIT_LOCKED_THIRDY_PARTY=ON"
  wh_ci_append_array_value "$array_name" "-DWH_THIRDY_PARTY_DIR=${WH_THIRDY_PARTY_DIR:-${WH_BUILD_ROOT}/thirdy_party}"
}

wh_ci_maybe_enable_ccache() {
  local array_name="$1"
  local prefix="${2:-ci}"

  if ! wh_has_command ccache; then
    return 0
  fi

  local arg=""
  while IFS= read -r arg; do
    [[ -n "$arg" ]] || continue
    wh_ci_append_array_value "$array_name" "$arg"
  done < <(wh_enable_ccache "$prefix")
}

wh_ci_configure_build_dir() {
  local build_dir="$1"
  shift

  wh_cmake_configure "$build_dir" "$(wh_default_cmake_generator)" "$@"
  wh_sync_compile_commands "$build_dir"
}

wh_ci_run_ctest_or_fail() {
  local tag="$1"
  local build_dir="$2"
  shift 2

  if wh_ctest_has_tests "$build_dir"; then
    ctest --test-dir "$build_dir" --output-on-failure --timeout 120 "$@"
    return 0
  fi

  wh_ci_fail "$tag" "no tests discovered in strict mode"
  exit 1
}

wh_ci_run_ctest_or_skip() {
  local tag="$1"
  local build_dir="$2"
  shift 2

  if wh_ctest_has_tests "$build_dir"; then
    ctest --test-dir "$build_dir" --output-on-failure --timeout 120 "$@"
    return 0
  fi

  if wh_is_strict_mode; then
    wh_ci_fail "$tag" "no tests discovered in strict mode"
    exit 1
  fi

  wh_ci_skip "$tag" "no tests"
  return 1
}
