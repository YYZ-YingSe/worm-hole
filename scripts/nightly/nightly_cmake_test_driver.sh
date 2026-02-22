#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

usage() {
  echo "usage: $0 sanitizer <asan|ubsan|tsan> | label <linearizability|stress|fuzz>"
}

has_tests() {
  local dir="$1"
  ctest --test-dir "$dir" -N 2>/dev/null | rg -q 'Total Tests:[[:space:]]*[1-9]'
}

has_labeled_tests() {
  local dir="$1"
  local label="$2"
  ctest --test-dir "$dir" -N -L "$label" 2>/dev/null | rg -q 'Total Tests:[[:space:]]*[1-9]'
}

configure_and_build() {
  local dir="$1"
  shift

  cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug "$@"
  cmake --build "$dir"
}

if [[ $# -ne 2 ]]; then
  usage
  exit 2
fi

mode="$1"
value="$2"

if [[ ! -f CMakeLists.txt ]]; then
  echo "[nightly] SKIP no CMakeLists.txt"
  exit 0
fi

if ! command -v cmake >/dev/null 2>&1 || ! command -v ctest >/dev/null 2>&1; then
  echo "[nightly] FAIL cmake/ctest required"
  exit 1
fi

case "$mode" in
  sanitizer)
    case "$value" in
      asan|ubsan|tsan) ;;
      *) usage; exit 2 ;;
    esac

    build_dir="build/nightly-${value}"
    san_flag="-fsanitize=${value} -fno-omit-frame-pointer"
    configure_and_build "$build_dir" \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_C_FLAGS="$san_flag" \
      -DCMAKE_CXX_FLAGS="$san_flag"

    if has_tests "$build_dir"; then
      ctest --test-dir "$build_dir" --output-on-failure
      echo "[nightly-${value}] PASS"
    else
      echo "[nightly-${value}] SKIP no tests"
    fi
    ;;

  label)
    build_dir="build/nightly-label"
    configure_and_build "$build_dir" \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++

    if has_labeled_tests "$build_dir" "$value"; then
      ctest --test-dir "$build_dir" --output-on-failure -L "$value"
      echo "[nightly-${value}] PASS"
    else
      echo "[nightly-${value}] SKIP no labeled tests"
    fi
    ;;

  *)
    usage
    exit 2
    ;;
esac
