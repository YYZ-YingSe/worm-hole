#!/usr/bin/env bash

if [[ -n "${WH_BUILD_CLI_SH_LOADED:-}" ]]; then
  return 0
fi
WH_BUILD_CLI_SH_LOADED=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/build/common.sh"
wh_enter_build_root

wh_build_cli_usage() {
  cat <<'EOF'
Usage: ./build.sh [options]

Actions:
  --configure                 Run cmake configure only
  --build                     Run build only (configure if needed)
  --test                      Run tests (configure+build if needed)
  --all                       Run configure + build + test
  --clean                     Remove selected build directory before action
  --clean-root                Remove the whole build root before action
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
  --enable-examples           Enable examples
  --disable-examples          Disable examples
  --enable-benchmarks         Enable benchmarks
  --disable-benchmarks        Disable benchmarks
  --werror                    Enable warnings-as-errors
  --no-werror                 Disable warnings-as-errors
  --strict-thirdy-party       Require git-locked thirdy_party deps
  --allow-unlocked-thirdy-party  Disable git lock enforcement
  --thirdy-party-dir <path>   thirdy_party root
  --stdexec-dir <path>        stdexec source directory override
  --rapidjson-dir <path>      rapidjson source directory override
  --catch2-dir <path>         catch2 source directory override
  --minja-dir <path>          minja source directory override
  --nlohmann-json-dir <path>  nlohmann_json source directory override

Test options:
  --list-tests                List tests only
  --ctest-filter <regex>      ctest -R regex
  --test-scope <scope>        all|ut|ft
  --ut-only                   Equivalent to --test-scope ut
  --ft-only                   Equivalent to --test-scope ft
  --coverage                  Configure with coverage flags
  --asan                      Enable AddressSanitizer
  --ubsan                     Enable UndefinedBehaviorSanitizer
  --tsan                      Enable ThreadSanitizer

Misc:
  --verbose                   Verbose test output
  -h, --help                  Show help
EOF
}

wh_build_cli_main() {
  local do_configure=0
  local do_build=0
  local do_test=0
  local do_clean=0
  local do_clean_root=0
  local do_reconfigure=0
  local do_sync_thirdy_party=0

  local build_type="Debug"
  local generator
  generator="$(wh_default_cmake_generator)"
  local build_dir=""
  local jobs=""
  local cxx="${CXX:-clang++}"

  local enable_tests="ON"
  local build_ut="ON"
  local build_ft="ON"
  local enable_examples="OFF"
  local enable_benchmarks="OFF"
  local warnings_as_errors="ON"
  local strict_thirdy_party="ON"
  local thirdy_party_dir="$ROOT/thirdy_party"
  local stdexec_dir=""
  local rapidjson_dir=""
  local catch2_dir=""
  local minja_dir=""
  local nlohmann_json_dir=""

  local want_coverage=0
  local want_asan=0
  local want_ubsan=0
  local want_tsan=0
  local list_tests=0
  local ctest_filter=""
  local verbose_test=0

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
      --clean-root)
        do_clean_root=1
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
      --enable-examples)
        enable_examples="ON"
        shift
        ;;
      --disable-examples)
        enable_examples="OFF"
        shift
        ;;
      --enable-benchmarks)
        enable_benchmarks="ON"
        shift
        ;;
      --disable-benchmarks)
        enable_benchmarks="OFF"
        shift
        ;;
      --werror)
        warnings_as_errors="ON"
        shift
        ;;
      --no-werror)
        warnings_as_errors="OFF"
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
      --minja-dir)
        minja_dir="$2"
        shift 2
        ;;
      --nlohmann-json-dir)
        nlohmann_json_dir="$2"
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
      --test-scope)
        case "$2" in
          all)
            build_ut="ON"
            build_ft="ON"
            ;;
          ut)
            build_ut="ON"
            build_ft="OFF"
            ;;
          ft)
            build_ut="OFF"
            build_ft="ON"
            ;;
          *)
            echo "[build] unknown test scope: $2" >&2
            wh_build_cli_usage
            exit 1
            ;;
        esac
        shift 2
        ;;
      --ut-only)
        build_ut="ON"
        build_ft="OFF"
        shift
        ;;
      --ft-only)
        build_ut="OFF"
        build_ft="ON"
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
        wh_build_cli_usage
        exit 0
        ;;
      *)
        echo "[build] unknown option: $1" >&2
        wh_build_cli_usage
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

  if [[ "$do_sync_thirdy_party" == "1" ]]; then
    if [[ -f .gitmodules ]]; then
      git submodule sync --recursive
      git submodule update --init --recursive
    else
      echo "[build] WARN .gitmodules not found, skip submodule sync"
    fi
  fi

  local sanitize_flags=""
  local build_variants=()
  if [[ "$want_asan" == "1" ]]; then
    sanitize_flags="${sanitize_flags},address"
    build_variants+=("asan")
  fi
  if [[ "$want_ubsan" == "1" ]]; then
    sanitize_flags="${sanitize_flags},undefined"
    build_variants+=("ubsan")
  fi
  if [[ "$want_tsan" == "1" ]]; then
    sanitize_flags="${sanitize_flags},thread"
    build_variants+=("tsan")
  fi
  sanitize_flags="${sanitize_flags#,}"

  local common_cxx_flags=""
  if [[ "$want_coverage" == "1" ]]; then
    common_cxx_flags="--coverage -O0 -g"
    build_variants+=("coverage")
  fi

  if [[ -z "$build_dir" ]]; then
    if [[ "${#build_variants[@]}" -eq 0 ]]; then
      build_dir="$(wh_default_manual_build_dir "$build_type")"
    else
      build_dir="$(wh_default_manual_build_dir "$build_type" "${build_variants[@]}")"
    fi
  fi

  if [[ "$do_clean_root" == "1" && -d "build" ]]; then
    rm -rf build
  fi

  if [[ "$do_clean" == "1" && -d "$build_dir" ]]; then
    rm -rf "$build_dir"
  fi

  if [[ -n "$sanitize_flags" ]]; then
    if [[ -n "$common_cxx_flags" ]]; then
      common_cxx_flags="$common_cxx_flags "
    fi
    common_cxx_flags="${common_cxx_flags}-fsanitize=${sanitize_flags} -fno-omit-frame-pointer"
  fi

  wh_build_cli_configure_once() {
    local cmake_args=(
      -DCMAKE_BUILD_TYPE="$build_type"
      -DCMAKE_CXX_COMPILER="$cxx"
      -DWH_BUILD_TESTING="$enable_tests"
      -DWH_BUILD_UT="$build_ut"
      -DWH_BUILD_FT="$build_ft"
      -DWH_BUILD_EXAMPLES="$enable_examples"
      -DWH_BUILD_BENCHMARKS="$enable_benchmarks"
      -DWH_WARNINGS_AS_ERRORS="$warnings_as_errors"
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
    if [[ -n "$common_cxx_flags" ]]; then
      cmake_args+=(-DCMAKE_CXX_FLAGS="$common_cxx_flags")
    fi
    if [[ -n "$minja_dir" ]]; then
      cmake_args+=(-DWH_MINJA_DIR="$minja_dir")
    fi
    if [[ -n "$nlohmann_json_dir" ]]; then
      cmake_args+=(-DWH_NLOHMANN_JSON_DIR="$nlohmann_json_dir")
    fi

    if wh_has_command ccache; then
      cmake_args+=("$(wh_enable_ccache "build")")
    fi

    wh_cmake_configure "$build_dir" "$generator" "${cmake_args[@]}"
    wh_sync_compile_commands "$build_dir"
  }

  if [[ "$do_configure" == "1" ]]; then
    wh_build_cli_configure_once
  fi

  if [[ "$do_build" == "1" || "$do_test" == "1" ]]; then
    if ! wh_has_build_system "$build_dir"; then
      wh_build_cli_configure_once
    fi

    wh_build_enabled_artifacts "$build_dir" "$jobs"
    wh_sync_compile_commands "$build_dir"
  fi

  if [[ "$do_test" == "1" || "$list_tests" == "1" ]]; then
    if [[ "$do_test" == "1" ]]; then
      if ! wh_ctest_has_tests "$build_dir"; then
        echo "[build] FAIL no tests discovered; check thirdy_party/catch2"
        exit 1
      fi
    fi

    local ctest_args=(--test-dir "$build_dir")
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

  wh_print_ccache_stats
  echo "[build] PASS"
}
