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
    packages=(ripgrep llvm cmake ninja)
    ;;
  *)
    echo "[install-macos-tools] FAIL unknown profile: $profile"
    exit 2
    ;;
esac

brew update
brew install "${packages[@]}"

llvm_bin="$(brew --prefix llvm)/bin"
if [[ -n "${GITHUB_PATH:-}" ]]; then
  printf '%s\n' "$llvm_bin" >> "$GITHUB_PATH"
fi

echo "[install-macos-tools] PASS $profile"
