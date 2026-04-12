#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "[install-macos-tools] FAIL usage: $0 <build>"
  exit 2
fi

profile="$1"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[install-macos-tools] FAIL macOS runner required"
  exit 1
fi

packages=()

case "$profile" in
  build)
    if ! brew list --versions llvm >/dev/null 2>&1; then
      packages+=(llvm)
    fi
    if ! command -v cmake >/dev/null 2>&1; then
      packages+=(cmake)
    fi
    if ! command -v ninja >/dev/null 2>&1; then
      packages+=(ninja)
    fi
    ;;
  *)
    echo "[install-macos-tools] FAIL unknown profile: $profile"
    exit 2
    ;;
esac

if [[ ${#packages[@]} -gt 0 ]]; then
  brew install "${packages[@]}"
fi

llvm_bin="$(brew --prefix llvm)/bin"
export PATH="$llvm_bin:$PATH"
if [[ -n "${GITHUB_PATH:-}" ]]; then
  printf '%s\n' "$llvm_bin" >> "$GITHUB_PATH"
fi

echo "[install-macos-tools] PASS $profile"
