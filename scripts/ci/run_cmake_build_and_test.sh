#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

strict_mode="${WH_CI_STRICT:-0}"
build_type="${WH_CI_BUILD_TYPE:-Debug}"
build_type_lc="$(printf '%s' "$build_type" | tr '[:upper:]' '[:lower:]')"
build_dir="build/ci-${build_type_lc}"

if [[ ! -f CMakeLists.txt ]]; then
  echo "[build-verify] SKIP no CMakeLists.txt"
  exit 0
fi

if ! command -v cmake >/dev/null 2>&1 || ! command -v ctest >/dev/null 2>&1; then
  if [[ "$strict_mode" == "1" || -n "${CI:-}" ]]; then
    echo "[build-verify] FAIL cmake/ctest required"
    exit 1
  fi
  echo "[build-verify] SKIP cmake/ctest not installed"
  exit 0
fi

cxx_compiler="${CXX:-clang++}"

warning_flags='-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor -Wold-style-cast'
if [[ "${WH_CI_WERROR:-1}" == "1" ]]; then
  warning_flags="$warning_flags -Werror"
fi

cmake -S . -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE="$build_type" \
  -DWH_BUILD_TESTING=ON \
  -DWH_REQUIRE_GIT_LOCKED_THIRDY_PARTY=ON \
  -DWH_THIRDY_PARTY_DIR="${WH_THIRDY_PARTY_DIR:-${ROOT}/thirdy_party}" \
  -DCMAKE_CXX_COMPILER="$cxx_compiler" \
  -DCMAKE_CXX_FLAGS="$warning_flags"

cmake --build "$build_dir" --parallel

if ctest --test-dir "$build_dir" -N 2>/dev/null | rg -q 'Total Tests:[[:space:]]*[1-9]'; then
  ctest --test-dir "$build_dir" --output-on-failure --timeout 120
  echo "[build-verify] PASS build+tests ($build_type)"
else
  if [[ "$strict_mode" == "1" || -n "${CI:-}" ]]; then
    echo "[build-verify] FAIL no tests discovered in strict mode ($build_type)"
    exit 1
  fi
  echo "[build-verify] PASS build only, no tests ($build_type)"
fi
