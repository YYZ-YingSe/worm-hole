#!/usr/bin/env bash
set -euo pipefail

selected_cc="clang"
selected_cxx="clang++"
selected_tidy="clang-tidy"
selected_llvm_cov="llvm-cov"
selected_llvm_profdata="llvm-profdata"
selected_version="default"

for ver in 20 19 18; do
  if command -v "clang-${ver}" >/dev/null 2>&1 && \
     command -v "clang++-${ver}" >/dev/null 2>&1 && \
     command -v "clang-tidy-${ver}" >/dev/null 2>&1; then
    selected_cc="clang-${ver}"
    selected_cxx="clang++-${ver}"
    selected_tidy="clang-tidy-${ver}"
    selected_version="${ver}"
    if command -v "llvm-cov-${ver}" >/dev/null 2>&1; then
      selected_llvm_cov="llvm-cov-${ver}"
    fi
    if command -v "llvm-profdata-${ver}" >/dev/null 2>&1; then
      selected_llvm_profdata="llvm-profdata-${ver}"
    fi
    break
  fi
done

if [[ -n "${GITHUB_ENV:-}" ]]; then
  {
    echo "CC=${selected_cc}"
    echo "CXX=${selected_cxx}"
    echo "WH_CLANG_TIDY_BIN=${selected_tidy}"
    echo "WH_LLVM_COV_BIN=${selected_llvm_cov}"
    echo "WH_LLVM_PROFDATA_BIN=${selected_llvm_profdata}"
    echo "WH_LLVM_VERSION=${selected_version}"
  } >> "$GITHUB_ENV"
fi

echo "[clang-tidy-toolchain] selected CC=${selected_cc} CXX=${selected_cxx} TIDY=${selected_tidy} LLVM_COV=${selected_llvm_cov} LLVM_PROFDATA=${selected_llvm_profdata} LLVM=${selected_version}"
