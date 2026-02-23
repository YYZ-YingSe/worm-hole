#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

if [[ "${RUNNER_OS:-}" == "Windows" ]] && ! command -v shellcheck >/dev/null 2>&1; then
  echo "[shellcheck] SKIP shellcheck not installed on windows runner"
  exit 0
fi

if ! command -v shellcheck >/dev/null 2>&1; then
  if [[ -n "${CI:-}" || "${WH_CI_STRICT:-0}" == "1" ]]; then
    echo "[shellcheck] FAIL shellcheck not installed"
    exit 1
  fi
  echo "[shellcheck] SKIP shellcheck not installed"
  exit 0
fi

script_listing="$(git ls-files 'scripts/**/*.sh')"
if [[ -z "$script_listing" ]]; then
  echo "[shellcheck] SKIP no shell scripts"
  exit 0
fi

scripts=()
while IFS= read -r path; do
  [[ -n "$path" ]] || continue
  scripts+=("$path")
done <<< "$script_listing"

shellcheck -x "${scripts[@]}"
echo "[shellcheck] PASS"
