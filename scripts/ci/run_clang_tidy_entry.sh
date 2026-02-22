#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

strict_mode="${WH_CI_STRICT:-0}"

if ! command -v clang-tidy >/dev/null 2>&1; then
  if [[ "$strict_mode" == "1" || -n "${CI:-}" ]]; then
    echo "[clang-tidy] FAIL clang-tidy not installed in strict mode"
    exit 1
  fi
  echo "[clang-tidy] SKIP clang-tidy not installed"
  exit 0
fi

source_listing="$(git ls-files '*.hpp' '*.h' '*.cpp' '*.cc' '*.cxx')"
if [[ -z "$source_listing" ]]; then
  echo "[clang-tidy] SKIP no source files"
  exit 0
fi

build_dir="build/ci-clang-tidy"
compile_db="$build_dir/compile_commands.json"

if [[ ! -f "$compile_db" ]]; then
  if [[ -f CMakeLists.txt ]] && command -v cmake >/dev/null 2>&1; then
    cmake -S . -B "$build_dir" -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
  else
    if [[ "$strict_mode" == "1" || -n "${CI:-}" ]]; then
      echo "[clang-tidy] FAIL compile_commands.json missing and cannot auto-generate"
      exit 1
    fi
    echo "[clang-tidy] SKIP compile_commands.json missing"
    exit 0
  fi
fi

checks="${WH_CLANG_TIDY_CHECKS:-clang-analyzer-*,bugprone-*,performance-*,portability-*,readability-*}"
header_filter='^(include/wh|src|tests)/'

source_files=()
while IFS= read -r path; do
  [[ -n "$path" ]] || continue
  source_files+=("$path")
done <<< "$source_listing"

clang-tidy -p "$build_dir" -checks="$checks" -header-filter="$header_filter" "${source_files[@]}"
echo "[clang-tidy] PASS"
