#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

export WH_CI_STRICT="${WH_CI_STRICT:-0}"

scripts/ci/guard_branch_policy.sh
scripts/ci/scan_secret_material.sh
scripts/ci/scan_naming.sh
scripts/ci/scan_blocking_calls.sh
scripts/ci/scan_api_signatures.sh
scripts/ci/scan_include_order.sh
scripts/ci/run_shellcheck.sh
scripts/ci/run_clang_format_check.sh
scripts/ci/run_clang_tidy_entry.sh
scripts/ci/run_cppcheck.sh
scripts/ci/run_cmake_build_and_test.sh
scripts/ci/run_clang_scan_build.sh

echo "[phase01-gates] PASS"
