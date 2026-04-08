#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$ROOT/scripts/ci/common.sh"

coverage_min="${WH_COVERAGE_MIN_LINES:-0.70}"

if ! wh_ci_require_cmake_project "coverage"; then
  exit 0
fi

if ! wh_ci_require_commands "coverage" cmake ctest gcovr; then
  exit 0
fi

build_dir="$(wh_ci_build_dir "coverage")"
cmake_args=(
  -DCMAKE_BUILD_TYPE=Debug
  -DWH_BUILD_TESTING=ON
  -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
  -DCMAKE_CXX_FLAGS="--coverage -O0 -g"
)

wh_ci_append_standard_third_party_args cmake_args
wh_ci_maybe_enable_ccache cmake_args "coverage"

wh_ci_configure_build_dir "$build_dir" "${cmake_args[@]}"
wh_build_test_artifacts "$build_dir" ""

wh_print_ccache_stats

if ! wh_ci_run_ctest_or_skip "coverage" "$build_dir"; then
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
elif wh_has_command llvm-cov; then
  gcov_exec="llvm-cov gcov"
fi

if [[ -n "$gcov_exec" ]]; then
  echo "[coverage] gcov executable: $gcov_exec"
  gcovr_args+=( --gcov-executable "$gcov_exec" )
fi

if ! gcovr "${gcovr_args[@]}"; then
  if wh_has_command gcov; then
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

line_rate_raw="$(
  rg -m1 -o 'line-rate="[0-9.]+"' "$report_xml" |
    sed -E 's/line-rate="([0-9.]+)"/\1/'
)"
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
