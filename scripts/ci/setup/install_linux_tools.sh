#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "[install-linux-tools] FAIL usage: $0 <fast-gates|build|deep-analysis|coverage|nightly>"
  exit 2
fi

profile="$1"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "[install-linux-tools] FAIL linux runner required"
  exit 1
fi

packages=()
versioned_llvm=0

case "$profile" in
  fast-gates)
    packages=(ripgrep shellcheck python3 python3-venv)
    ;;
  build)
    packages=(ripgrep clang cmake ninja-build ccache)
    ;;
  deep-analysis)
    packages=(ripgrep clang clang-tidy clang-tools cppcheck cmake ninja-build)
    versioned_llvm=1
    ;;
  coverage)
    packages=(ripgrep clang cmake ninja-build gcovr ccache)
    ;;
  nightly)
    packages=(ripgrep clang clang-tools cmake ninja-build)
    ;;
  *)
    echo "[install-linux-tools] FAIL unknown profile: $profile"
    exit 2
    ;;
esac

sudo apt-get update
sudo apt-get install -y "${packages[@]}"

if [[ "$versioned_llvm" == "1" ]]; then
  for ver in 20 19 18; do
    if apt-cache show "clang-${ver}" >/dev/null 2>&1 && \
       apt-cache show "clang-tidy-${ver}" >/dev/null 2>&1 && \
       apt-cache show "clang-tools-${ver}" >/dev/null 2>&1; then
      sudo apt-get install -y \
        "clang-${ver}" \
        "clang-tidy-${ver}" \
        "clang-tools-${ver}"
    fi
  done
fi

echo "[install-linux-tools] PASS $profile"
