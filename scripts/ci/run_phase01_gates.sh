#!/usr/bin/env bash
set -euo pipefail

# Public local entrypoint for the full phase01 gate sequence.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

export WH_CI_STRICT="${WH_CI_STRICT:-0}"

bash scripts/ci/jobs/run_fast_gates.sh
bash scripts/ci/jobs/run_static_analysis_suite.sh
bash scripts/ci/jobs/run_cmake_build_and_test.sh

echo "[phase01-gates] PASS"
