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

source_listing="$(git ls-files '*.cpp' '*.cc' '*.cxx' | rg -v '^thirdy_party/' || true)"
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
header_filter='^(include/wh|src|tests)/'
tidy_config='{InheritParentConfig: false}'

echo "[clang-tidy] binary: ${clang_tidy_bin}"
"$clang_tidy_bin" --version | head -n 1

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

"$clang_tidy_bin" \
  -p "$build_dir" \
  -checks="$checks" \
  -header-filter="$header_filter" \
  --config="$tidy_config" \
  "${source_files[@]}"
echo "[clang-tidy] PASS"
