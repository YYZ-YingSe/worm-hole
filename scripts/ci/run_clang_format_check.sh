#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

source_listing="$(git ls-files '*.hpp' '*.h' '*.cpp' '*.cc' '*.cxx' '*.ipp')"
if [[ -z "$source_listing" ]]; then
  echo "[clang-format] SKIP no source files"
  exit 0
fi

if ! command -v clang-format >/dev/null 2>&1; then
  if [[ -n "${CI:-}" ]]; then
    echo "[clang-format] FAIL clang-format not installed in CI"
    exit 1
  fi
  echo "[clang-format] SKIP clang-format not installed"
  exit 0
fi

source_files=()
while IFS= read -r path; do
  [[ -n "$path" ]] || continue
  source_files+=("$path")
done <<< "$source_listing"

clang-format --dry-run --Werror "${source_files[@]}"
echo "[clang-format] PASS"
