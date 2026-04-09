#!/usr/bin/env bash
set -euo pipefail

selected_cc="clang"
selected_cxx="clang++"
selected_tidy="clang-tidy"
selected_scan_build="scan-build"
selected_version="default"

for ver in 20 19 18; do
  if command -v "clang-${ver}" >/dev/null 2>&1 && \
     command -v "clang++-${ver}" >/dev/null 2>&1 && \
     command -v "clang-tidy-${ver}" >/dev/null 2>&1; then
    selected_cc="clang-${ver}"
    selected_cxx="clang++-${ver}"
    selected_tidy="clang-tidy-${ver}"
    selected_version="${ver}"

    if command -v "scan-build-${ver}" >/dev/null 2>&1; then
      selected_scan_build="scan-build-${ver}"
    fi
    break
  fi
done

if [[ -n "${GITHUB_ENV:-}" ]]; then
  {
    echo "CC=${selected_cc}"
    echo "CXX=${selected_cxx}"
    echo "WH_CLANG_TIDY_BIN=${selected_tidy}"
    echo "WH_SCAN_BUILD_BIN=${selected_scan_build}"
    echo "WH_LLVM_VERSION=${selected_version}"
  } >> "$GITHUB_ENV"
fi

echo "[clang-tidy-toolchain] selected CC=${selected_cc} CXX=${selected_cxx} TIDY=${selected_tidy} SCAN_BUILD=${selected_scan_build} LLVM=${selected_version}"
