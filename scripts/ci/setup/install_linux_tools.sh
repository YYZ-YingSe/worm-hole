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
preferred_llvm_version=""

case "$profile" in
  fast-gates)
    packages=(ripgrep shellcheck python3 python3-venv)
    ;;
  build)
    packages=(ripgrep clang cmake ninja-build ccache)
    ;;
  deep-analysis)
    packages=(ripgrep cppcheck cmake ninja-build)
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
if [[ "${#packages[@]}" -gt 0 ]]; then
  sudo apt-get install -y "${packages[@]}"
fi

if [[ "$profile" == "deep-analysis" ]]; then
  for ver in 20 19 18; do
    if apt-cache show "clang-${ver}" >/dev/null 2>&1 && \
       apt-cache show "clang-tidy-${ver}" >/dev/null 2>&1 && \
       apt-cache show "clang-tools-${ver}" >/dev/null 2>&1; then
      preferred_llvm_version="$ver"
      break
    fi
  done

  if [[ -n "$preferred_llvm_version" ]]; then
    sudo apt-get install -y \
      "clang-${preferred_llvm_version}" \
      "clang-tidy-${preferred_llvm_version}" \
      "clang-tools-${preferred_llvm_version}"
  else
    sudo apt-get install -y clang clang-tidy clang-tools
  fi
fi

echo "[install-linux-tools] PASS $profile"
