#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

strict_mode="${WH_CI_STRICT:-0}"

if ! command -v cppcheck >/dev/null 2>&1; then
  if [[ "$strict_mode" == "1" || -n "${CI:-}" ]]; then
    echo "[cppcheck] FAIL cppcheck not installed in strict mode"
    exit 1
  fi
  echo "[cppcheck] SKIP cppcheck not installed"
  exit 0
fi

source_listing="$(git ls-files 'include/wh/**/*.hpp' 'include/wh/**/*.h' 'src/**/*.cpp' 'src/**/*.cc' 'src/**/*.cxx' 'tests/**/*.cpp' 'tests/**/*.cc' 'tests/**/*.cxx' 2>/dev/null || true)"
if [[ -z "$source_listing" ]]; then
  echo "[cppcheck] SKIP no source files"
  exit 0
fi

source_files=()
while IFS= read -r path; do
  [[ -n "$path" ]] || continue
  source_files+=("$path")
done <<< "$source_listing"

cppcheck \
  --enable=warning,performance,portability \
  --error-exitcode=2 \
  --language=c++ \
  --std=c++20 \
  --quiet \
  "${source_files[@]}"

echo "[cppcheck] PASS"
