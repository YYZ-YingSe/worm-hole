#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"

export WH_CI_STRICT="${WH_CI_STRICT:-0}"

bash scripts/ci/jobs/run_clang_tidy_entry.sh
bash scripts/ci/checks/run_cppcheck.sh
bash scripts/ci/jobs/run_clang_scan_build.sh

echo "[static-analysis-suite] PASS"
