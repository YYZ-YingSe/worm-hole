#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

usage() {
  cat <<'EOF'
Usage: ./build.sh [options]

Actions:
  --configure                 Run cmake configure only
  --build                     Run build only (configure if needed)
  --test                      Run tests (configure+build if needed)
  --all                       Run configure + build + test
  --clean                     Remove build directory before action
  --reconfigure               Clean then configure
  --sync-thirdy-party         Run git submodule update --init --recursive

Build options:
  --build-type <type>         Debug|Release|RelWithDebInfo|MinSizeRel
  --build-dir <path>          Build directory
  --generator <name>          CMake generator (default: Ninja)
  --jobs <N>                  Parallel build jobs
  --cxx <path>                C++ compiler

Feature options:
  --enable-tests              Enable tests
  --disable-tests             Disable tests
  --strict-thirdy-party       Require git-locked thirdy_party deps
  --allow-unlocked-thirdy-party  Disable git lock enforcement
  --thirdy-party-dir <path>   thirdy_party root
  --stdexec-dir <path>        stdexec source directory override
  --rapidjson-dir <path>      rapidjson source directory override
  --catch2-dir <path>         catch2 source directory override

Test options:
  --list-tests                List tests only
  --ctest-filter <regex>      ctest -R regex
  --coverage                  Configure with coverage flags
  --asan                      Enable AddressSanitizer
  --ubsan                     Enable UndefinedBehaviorSanitizer
  --tsan                      Enable ThreadSanitizer

Misc:
  --verbose                   Verbose test output
  -h, --help                  Show help
EOF
}

do_configure=0
do_build=0
do_test=0
do_clean=0
do_reconfigure=0
do_sync_thirdy_party=0

build_type="Debug"
generator="Ninja"
build_dir=""
jobs=""
cxx="${CXX:-clang++}"

enable_tests="ON"
strict_thirdy_party="ON"
thirdy_party_dir="$ROOT/thirdy_party"
stdexec_dir=""
rapidjson_dir=""
catch2_dir=""

want_coverage=0
want_asan=0
want_ubsan=0
want_tsan=0
list_tests=0
ctest_filter=""
verbose_test=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --configure)
      do_configure=1
      shift
      ;;
    --build)
      do_build=1
      shift
      ;;
    --test)
      do_test=1
      shift
      ;;
    --all)
      do_configure=1
      do_build=1
      do_test=1
      shift
      ;;
    --clean)
      do_clean=1
      shift
      ;;
    --reconfigure)
      do_reconfigure=1
      do_configure=1
      shift
      ;;
    --sync-thirdy-party)
      do_sync_thirdy_party=1
      shift
      ;;
    --build-type)
      build_type="$2"
      shift 2
      ;;
    --build-dir)
      build_dir="$2"
      shift 2
      ;;
    --generator)
      generator="$2"
      shift 2
      ;;
    --jobs)
      jobs="$2"
      shift 2
      ;;
    --cxx)
      cxx="$2"
      shift 2
      ;;
    --enable-tests)
      enable_tests="ON"
      shift
      ;;
    --disable-tests)
      enable_tests="OFF"
      shift
      ;;
    --strict-thirdy-party)
      strict_thirdy_party="ON"
      shift
      ;;
    --allow-unlocked-thirdy-party)
      strict_thirdy_party="OFF"
      shift
      ;;
    --thirdy-party-dir)
      thirdy_party_dir="$2"
      shift 2
      ;;
    --stdexec-dir)
      stdexec_dir="$2"
      shift 2
      ;;
    --rapidjson-dir)
      rapidjson_dir="$2"
      shift 2
      ;;
    --catch2-dir)
      catch2_dir="$2"
      shift 2
      ;;
    --coverage)
      want_coverage=1
      shift
      ;;
    --asan)
      want_asan=1
      shift
      ;;
    --ubsan)
      want_ubsan=1
      shift
      ;;
    --tsan)
      want_tsan=1
      shift
      ;;
    --list-tests)
      list_tests=1
      shift
      ;;
    --ctest-filter)
      ctest_filter="$2"
      shift 2
      ;;
    --verbose)
      verbose_test=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[build] unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ "$do_reconfigure" == "1" ]]; then
  do_clean=1
fi

if [[ "$do_configure" == "0" && "$do_build" == "0" && "$do_test" == "0" ]]; then
  do_configure=1
  do_build=1
fi

build_type_lc="$(printf '%s' "$build_type" | tr '[:upper:]' '[:lower:]')"
if [[ -z "$build_dir" ]]; then
  build_dir="build/local-${build_type_lc}"
fi

if [[ "$do_sync_thirdy_party" == "1" ]]; then
  if [[ -f .gitmodules ]]; then
    git submodule sync --recursive
    git submodule update --init --recursive
  else
    echo "[build] WARN .gitmodules not found, skip submodule sync"
  fi
fi

if [[ "$do_clean" == "1" && -d "$build_dir" ]]; then
  rm -rf "$build_dir"
fi

sanitize_flags=""
if [[ "$want_asan" == "1" ]]; then
  sanitize_flags="${sanitize_flags},address"
fi
if [[ "$want_ubsan" == "1" ]]; then
  sanitize_flags="${sanitize_flags},undefined"
fi
if [[ "$want_tsan" == "1" ]]; then
  sanitize_flags="${sanitize_flags},thread"
fi
sanitize_flags="${sanitize_flags#,}"

common_cxx_flags="-Wall -Wextra -Wpedantic"
if [[ "$want_coverage" == "1" ]]; then
  common_cxx_flags="$common_cxx_flags --coverage -O0 -g"
fi
if [[ -n "$sanitize_flags" ]]; then
  common_cxx_flags="$common_cxx_flags -fsanitize=${sanitize_flags} -fno-omit-frame-pointer"
fi

configure_once() {
  cmake_args=(
    -S .
    -B "$build_dir"
    -G "$generator"
    -DCMAKE_BUILD_TYPE="$build_type"
    -DCMAKE_CXX_COMPILER="$cxx"
    -DCMAKE_CXX_FLAGS="$common_cxx_flags"
    -DWH_BUILD_TESTING="$enable_tests"
    -DWH_REQUIRE_GIT_LOCKED_THIRDY_PARTY="$strict_thirdy_party"
    -DWH_THIRDY_PARTY_DIR="$thirdy_party_dir"
  )

  if [[ -n "$stdexec_dir" ]]; then
    cmake_args+=(-DWH_STDEXEC_DIR="$stdexec_dir")
  fi
  if [[ -n "$rapidjson_dir" ]]; then
    cmake_args+=(-DWH_RAPIDJSON_DIR="$rapidjson_dir")
  fi
  if [[ -n "$catch2_dir" ]]; then
    cmake_args+=(-DWH_CATCH2_DIR="$catch2_dir")
  fi

  cmake "${cmake_args[@]}"
}

if [[ "$do_configure" == "1" ]]; then
  configure_once
fi

if [[ "$do_build" == "1" || "$do_test" == "1" ]]; then
  if [[ ! -f "$build_dir/build.ninja" && ! -f "$build_dir/Makefile" ]]; then
    configure_once
  fi

  if [[ -n "$jobs" ]]; then
    cmake --build "$build_dir" --parallel "$jobs"
  else
    cmake --build "$build_dir" --parallel
  fi
fi

if [[ "$do_test" == "1" || "$list_tests" == "1" ]]; then
  if [[ "$do_test" == "1" ]]; then
    if ! ctest --test-dir "$build_dir" -N 2>/dev/null | rg -q 'Total Tests:[[:space:]]*[1-9]'; then
      echo "[build] FAIL no tests discovered; check thirdy_party/catch2"
      exit 1
    fi
  fi

  ctest_args=(--test-dir "$build_dir")
  if [[ "$list_tests" == "1" ]]; then
    ctest_args+=(-N)
  else
    ctest_args+=(--output-on-failure --timeout 120)
    if [[ "$verbose_test" == "1" ]]; then
      ctest_args+=(-V)
    fi
  fi
  if [[ -n "$ctest_filter" ]]; then
    ctest_args+=(-R "$ctest_filter")
  fi
  ctest "${ctest_args[@]}"
fi

echo "[build] PASS"
