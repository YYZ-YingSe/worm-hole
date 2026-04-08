#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${GITHUB_PATH:-}" ]]; then
  echo "[setup-clang-format] FAIL GITHUB_PATH is required"
  exit 1
fi

if [[ -z "${GITHUB_ENV:-}" ]]; then
  echo "[setup-clang-format] FAIL GITHUB_ENV is required"
  exit 1
fi

venv_dir="${WH_CLANG_FORMAT_VENV_DIR:-.cache/ci/clang-format}"

python3 -m venv "$venv_dir"
source "$venv_dir/bin/activate"

if [[ ! -x "$venv_dir/bin/clang-format" ]]; then
  python -m pip install clang-format==21.1.8
fi

venv_bin_dir="$venv_dir/bin"
case "$venv_bin_dir" in
  /*)
    printf '%s\n' "$venv_bin_dir" >> "$GITHUB_PATH"
    ;;
  *)
    printf '%s\n' "$PWD/$venv_bin_dir" >> "$GITHUB_PATH"
    ;;
esac

printf '%s=%s\n' "WH_CLANG_FORMAT_BIN" "clang-format" >> "$GITHUB_ENV"

echo "[setup-clang-format] PASS $venv_dir"
