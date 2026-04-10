#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${GITHUB_PATH:-}" ]]; then
  echo "[setup-codechecker] FAIL GITHUB_PATH is required"
  exit 1
fi

if [[ -z "${GITHUB_ENV:-}" ]]; then
  echo "[setup-codechecker] FAIL GITHUB_ENV is required"
  exit 1
fi

venv_dir="${WH_CODECHECKER_VENV_DIR:-.cache/ci/codechecker}"
codechecker_version="${WH_CODECHECKER_VERSION:-6.27.3}"

if [[ ! -x "$venv_dir/bin/CodeChecker" && ! -x "$venv_dir/bin/codechecker" ]]; then
  python3 -m venv "$venv_dir"

  # shellcheck source=/dev/null
  source "$venv_dir/bin/activate"
  python -m pip install --upgrade pip
  python -m pip install "codechecker==${codechecker_version}"
else
  # shellcheck source=/dev/null
  source "$venv_dir/bin/activate"
fi

codechecker_bin="$venv_dir/bin/CodeChecker"
if [[ ! -x "$codechecker_bin" ]]; then
  codechecker_bin="$venv_dir/bin/codechecker"
fi

if [[ ! -x "$codechecker_bin" ]]; then
  echo "[setup-codechecker] FAIL CodeChecker binary not found in $venv_dir"
  exit 1
fi

printf '%s\n' "$venv_dir/bin" >> "$GITHUB_PATH"
printf '%s=%s\n' "WH_CODECHECKER_BIN" "$codechecker_bin" >> "$GITHUB_ENV"

echo "[setup-codechecker] PASS $venv_dir"
