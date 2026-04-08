#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"

export WH_CI_STRICT="${WH_CI_STRICT:-0}"

bash scripts/ci/checks/guard_branch_policy.sh
bash scripts/ci/checks/scan_secret_material.sh
bash scripts/ci/checks/scan_include_order.sh
bash scripts/ci/checks/run_shellcheck.sh
bash scripts/ci/checks/run_clang_format_check.sh

echo "[fast-gates] PASS"
