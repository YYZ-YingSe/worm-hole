#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$ROOT/scripts/ci/common.sh"

if [[ "${RUNNER_OS:-}" == "macOS" ]]; then
  echo "[clang-tidy] SKIP macOS runner (toolchain mismatch risk); enforced on ubuntu-latest"
  exit 0
fi

if [[ "${RUNNER_OS:-}" == "Windows" ]]; then
  echo "[clang-tidy] SKIP windows runner (compile db/toolchain mismatch risk); enforced on ubuntu-latest"
  exit 0
fi

clang_tidy_bin="${WH_CLANG_TIDY_BIN:-clang-tidy}"

if ! wh_ci_require_commands "clang-tidy" "$clang_tidy_bin"; then
  exit 0
fi

detect_tidy_jobs() {
  if [[ -n "${WH_CLANG_TIDY_JOBS:-}" ]]; then
    printf '%s\n' "${WH_CLANG_TIDY_JOBS}"
    return
  fi

  if command -v nproc >/dev/null 2>&1; then
    nproc
    return
  fi

  if command -v getconf >/dev/null 2>&1; then
    getconf _NPROCESSORS_ONLN
    return
  fi

  if command -v sysctl >/dev/null 2>&1; then
    sysctl -n hw.ncpu
    return
  fi

  printf '4\n'
}

path_filter="${WH_CLANG_TIDY_PATH_FILTER:-^(tests/UT/|tests/helper/)}"

source_listing="$(
  git ls-files '*.cpp' '*.cc' '*.cxx' |
    rg "$path_filter" || true
)"
if [[ -z "$source_listing" ]]; then
  echo "[clang-tidy] SKIP no source files"
  exit 0
fi

build_dir="$(wh_ci_build_dir "clang-tidy")"
compile_db="$build_dir/compile_commands.json"

if [[ ! -f "$compile_db" ]]; then
  if ! wh_ci_require_cmake_project "clang-tidy"; then
    exit 0
  fi

  if wh_has_command cmake; then
    cmake_args=(
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
      -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
      -DWH_BUILD_TESTING=ON
      -DWH_WARNINGS_AS_ERRORS=OFF
    )
    wh_ci_append_standard_third_party_args cmake_args
    wh_ci_configure_build_dir "$build_dir" "${cmake_args[@]}" >/dev/null
  else
    if wh_is_strict_mode; then
      echo "[clang-tidy] FAIL compile_commands.json missing and cannot auto-generate"
      exit 1
    fi
    echo "[clang-tidy] SKIP compile_commands.json missing"
    exit 0
  fi
fi

wh_sync_compile_commands "$build_dir"

checks="${WH_CLANG_TIDY_CHECKS:-clang-analyzer-*,bugprone-*,performance-*,portability-*,readability-*}"
header_filter='^(include/wh|tests/UT|tests/helper)/'
tidy_config='{InheritParentConfig: false}'
tidy_jobs="$(detect_tidy_jobs)"

echo "[clang-tidy] binary: ${clang_tidy_bin}"
"$clang_tidy_bin" --version | head -n 1
echo "[clang-tidy] scope filter: ${path_filter}"
echo "[clang-tidy] jobs: ${tidy_jobs}"

source_files=()
while IFS= read -r path; do
  [[ -n "$path" ]] || continue
  [[ -f "$path" ]] || continue
  source_files+=("$path")
done <<< "$source_listing"

if [[ "${#source_files[@]}" -eq 0 ]]; then
  echo "[clang-tidy] SKIP no existing source files"
  exit 0
fi

tidy_args=(
  -p "$build_dir"
  -checks="$checks"
  -header-filter="$header_filter"
  --config="$tidy_config"
  --quiet
)

if [[ "$tidy_jobs" =~ ^[1-9][0-9]*$ ]] && (( tidy_jobs > 1 )); then
  printf '%s\n' "${source_files[@]}" |
    xargs -P "$tidy_jobs" -n 1 "$clang_tidy_bin" "${tidy_args[@]}"
else
  "$clang_tidy_bin" "${tidy_args[@]}" "${source_files[@]}"
fi

echo "[clang-tidy] PASS"
