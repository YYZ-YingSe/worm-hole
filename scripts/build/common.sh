#!/usr/bin/env bash

if [[ -n "${WH_BUILD_COMMON_SH_LOADED:-}" ]]; then
  return 0
fi
WH_BUILD_COMMON_SH_LOADED=1

WH_BUILD_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

wh_enter_build_root() {
  cd "$WH_BUILD_ROOT" || return 1
}

wh_lowercase() {
  printf '%s' "$1" | tr '[:upper:]' '[:lower:]'
}

wh_build_dir_for_scope() {
  local scope="$1"
  shift

  local path="build"
  if [[ -n "$scope" ]]; then
    path+="/$scope"
  fi

  local segment=""
  for segment in "$@"; do
    [[ -n "$segment" ]] || continue
    path+="/$segment"
  done

  printf '%s\n' "$path"
}

wh_default_manual_build_dir() {
  local build_type="${1:-Debug}"
  shift || true

  local profile
  profile="$(wh_lowercase "$build_type")"
  local variant=""
  for variant in "$@"; do
    [[ -n "$variant" ]] || continue
    profile+="-${variant}"
  done

  wh_build_dir_for_scope "manual" "$profile"
}

wh_default_manual_env_build_dir() {
  local build_type="${WH_BUILD_TYPE:-Debug}"
  wh_default_manual_build_dir "$build_type" "${WH_BUILD_VARIANT:-}"
}

wh_ci_build_dir() {
  wh_build_dir_for_scope "ci" "$@"
}

wh_nightly_build_dir() {
  wh_build_dir_for_scope "nightly" "$@"
}

wh_default_cmake_generator() {
  printf '%s\n' "${WH_CMAKE_GENERATOR:-Ninja}"
}

wh_has_cmake_project() {
  [[ -f "$WH_BUILD_ROOT/CMakeLists.txt" ]]
}

wh_has_command() {
  command -v "$1" >/dev/null 2>&1
}

wh_is_strict_mode() {
  [[ "${WH_CI_STRICT:-0}" == "1" || -n "${CI:-}" ]]
}

wh_has_build_system() {
  local build_dir="$1"
  [[ -f "$build_dir/build.ninja" || -f "$build_dir/Makefile" ]]
}

wh_enable_ccache() {
  local prefix="${1:-build}"
  echo "[${prefix}] ccache enabled" >&2
  ccache -z >/dev/null 2>&1 || true
  printf '%s\n' "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
}

wh_print_ccache_stats() {
  if wh_has_command ccache; then
    ccache -s || true
  fi
}

wh_sync_compile_commands() {
  local build_dir="$1"
  local source_file="${build_dir%/}/compile_commands.json"
  local target_file="$WH_BUILD_ROOT/compile_commands.json"

  if [[ ! -f "$source_file" ]]; then
    echo "[compile-commands] SKIP missing $source_file"
    return 0
  fi

  cp "$source_file" "$target_file"
  echo "[compile-commands] SYNC $source_file -> $target_file"
}

wh_cmake_configure() {
  local build_dir="$1"
  local generator="$2"
  shift 2
  cmake -S "$WH_BUILD_ROOT" -B "$build_dir" -G "$generator" "$@"
}

wh_cmake_build() {
  local build_dir="$1"
  local jobs="${2:-}"

  if [[ -n "$jobs" ]]; then
    cmake --build "$build_dir" --parallel "$jobs"
    return
  fi

  cmake --build "$build_dir" --parallel
}

wh_target_enabled_artifacts() {
  printf '%s\n' "wh_enabled_artifacts"
}

wh_target_test_artifacts() {
  printf '%s\n' "wh_test_artifacts"
}

wh_cmake_build_targets() {
  local build_dir="$1"
  local jobs="${2:-}"
  shift 2

  local targets=("$@")
  if [[ "${#targets[@]}" -eq 0 ]]; then
    wh_cmake_build "$build_dir" "$jobs"
    return
  fi

  if [[ -n "$jobs" ]]; then
    cmake --build "$build_dir" --parallel "$jobs" --target "${targets[@]}"
    return
  fi

  cmake --build "$build_dir" --parallel --target "${targets[@]}"
}

wh_build_enabled_artifacts() {
  local build_dir="$1"
  local jobs="${2:-}"
  wh_cmake_build_targets "$build_dir" "$jobs" "$(wh_target_enabled_artifacts)"
}

wh_build_test_artifacts() {
  local build_dir="$1"
  local jobs="${2:-}"
  wh_cmake_build_targets "$build_dir" "$jobs" "$(wh_target_test_artifacts)"
}

wh_ctest_has_tests() {
  local build_dir="$1"
  ctest --test-dir "$build_dir" -N 2>/dev/null |
    grep -Eq 'Total Tests:[[:space:]]*[1-9]'
}

wh_ctest_has_label_tests() {
  local build_dir="$1"
  local label="$2"
  ctest --test-dir "$build_dir" -N -L "$label" 2>/dev/null |
    grep -Eq 'Total Tests:[[:space:]]*[1-9]'
}
