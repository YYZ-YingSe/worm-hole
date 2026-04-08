#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/build/common.sh"
wh_enter_build_root

build_dir="${WH_BUILD_DIR:-$(wh_default_manual_env_build_dir)}"
jobs="${WH_BUILD_JOBS:-}"

if ! wh_has_build_system "$build_dir"; then
  bash scripts/build/configure.sh
fi

wh_build_enabled_artifacts "$build_dir" "$jobs"
wh_sync_compile_commands "$build_dir"
wh_print_ccache_stats
echo "[build] PASS $build_dir"
