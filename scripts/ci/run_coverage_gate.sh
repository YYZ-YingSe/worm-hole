#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

strict_mode="${WH_CI_STRICT:-0}"
coverage_min="${WH_COVERAGE_MIN_LINES:-0.70}"

if [[ ! -f CMakeLists.txt ]]; then
  echo "[coverage] SKIP no CMakeLists.txt"
  exit 0
fi

required_cmds=(cmake ctest gcovr)
for cmd in "${required_cmds[@]}"; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    if [[ "$strict_mode" == "1" || -n "${CI:-}" ]]; then
      echo "[coverage] FAIL missing required tool: $cmd"
      exit 1
    fi
    echo "[coverage] SKIP missing required tool: $cmd"
    exit 0
  fi
done

build_dir="build/coverage"
cmake_args=(
  -DCMAKE_BUILD_TYPE=Debug
  -DWH_BUILD_TESTING=ON
  -DWH_REQUIRE_GIT_LOCKED_THIRDY_PARTY=ON
  -DWH_THIRDY_PARTY_DIR="${WH_THIRDY_PARTY_DIR:-${ROOT}/thirdy_party}"
  -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
  -DCMAKE_CXX_FLAGS="--coverage -O0 -g"
)

if command -v ccache >/dev/null 2>&1; then
  echo "[coverage] ccache enabled"
  ccache -z >/dev/null 2>&1 || true
  cmake_args+=( -DCMAKE_CXX_COMPILER_LAUNCHER=ccache )
fi

cmake -S . -B "$build_dir" -G Ninja "${cmake_args[@]}"
cmake --build "$build_dir" --parallel

if command -v ccache >/dev/null 2>&1; then
  ccache -s || true
fi

if ctest --test-dir "$build_dir" -N 2>/dev/null | rg -q 'Total Tests:[[:space:]]*[1-9]'; then
  ctest --test-dir "$build_dir" --output-on-failure --timeout 120
else
  if [[ "$strict_mode" == "1" || -n "${CI:-}" ]]; then
    echo "[coverage] FAIL no tests discovered in strict mode"
    exit 1
  fi
  echo "[coverage] SKIP no tests"
  exit 0
fi

report_dir="$build_dir/reports"
mkdir -p "$report_dir"
report_xml="$report_dir/coverage.xml"

gcovr_args=(
  --root .
  --xml-pretty
  --output "$report_xml"
  --exclude '(^|/)thirdy_party/'
  --exclude-directories '(.*/)?thirdy_party(/.*)?'
  --gcov-ignore-errors all
  -j 1
  --print-summary
)

gcov_exec=""
cxx_major=""
if command -v "${CXX:-clang++}" >/dev/null 2>&1; then
  cxx_major="$("${CXX:-clang++}" --version | rg -o '[0-9]+' | head -n1 || true)"
fi
if [[ -n "$cxx_major" ]] && command -v "llvm-cov-$cxx_major" >/dev/null 2>&1; then
  gcov_exec="llvm-cov-$cxx_major gcov"
elif command -v llvm-cov >/dev/null 2>&1; then
  gcov_exec="llvm-cov gcov"
fi

if [[ -n "$gcov_exec" ]]; then
  echo "[coverage] gcov executable: $gcov_exec"
  gcovr_args+=( --gcov-executable "$gcov_exec" )
fi

if ! gcovr "${gcovr_args[@]}"; then
  if command -v gcov >/dev/null 2>&1; then
    echo "[coverage] WARN gcovr failed with ${gcov_exec:-default}; retrying with gcov"
    gcovr "${gcovr_args[@]}" --gcov-executable gcov
  else
    exit 1
  fi
fi

if [[ ! -f "$report_xml" ]]; then
  echo "[coverage] FAIL missing coverage report: $report_xml"
  exit 1
fi

line_rate_raw="$(rg -o 'line-rate="[0-9.]+"' "$report_xml" | head -n1 | sed -E 's/line-rate="([0-9.]+)"/\1/')"
if [[ -z "$line_rate_raw" ]]; then
  echo "[coverage] FAIL cannot parse line-rate from report"
  exit 1
fi

line_pct="$(awk -v r="$line_rate_raw" 'BEGIN { printf "%.2f", r*100 }')"
min_pct="$(awk -v r="$coverage_min" 'BEGIN { printf "%.2f", r*100 }')"

if awk -v cur="$line_rate_raw" -v min="$coverage_min" 'BEGIN { exit !(cur + 1e-9 >= min) }'; then
  echo "[coverage] PASS line coverage ${line_pct}% (threshold ${min_pct}%)"
else
  echo "[coverage] FAIL line coverage ${line_pct}% below threshold ${min_pct}%"
  exit 1
fi
