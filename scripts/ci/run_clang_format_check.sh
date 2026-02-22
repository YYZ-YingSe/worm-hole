#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

source_listing="$(git ls-files '*.hpp' '*.h' '*.cpp' '*.cc' '*.cxx' '*.ipp' | rg -v '^thirdy_party/' || true)"
if [[ -z "$source_listing" ]]; then
  echo "[clang-format] SKIP no source files"
  exit 0
fi

formatter_bin="${WH_CLANG_FORMAT_BIN:-clang-format}"

if ! command -v "$formatter_bin" >/dev/null 2>&1; then
  if [[ -n "${CI:-}" ]]; then
    echo "[clang-format] FAIL clang-format not installed in CI"
    exit 1
  fi
  echo "[clang-format] SKIP clang-format not installed"
  exit 0
fi

if [[ ! -f .clang-format ]]; then
  echo "[clang-format] FAIL missing .clang-format in repo root"
  exit 1
fi

echo "[clang-format] using: $formatter_bin"
"$formatter_bin" --version

source_files=()
while IFS= read -r path; do
  [[ -n "$path" ]] || continue
  source_files+=("$path")
done <<< "$source_listing"

"$formatter_bin" --style=file --fallback-style=none --dry-run --Werror "${source_files[@]}"
echo "[clang-format] PASS"
