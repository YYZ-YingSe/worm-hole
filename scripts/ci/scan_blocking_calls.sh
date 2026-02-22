#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

status=0
pattern='std::mutex|std::recursive_mutex|std::condition_variable|std::condition_variable_any|std::jthread|std::this_thread::sleep_for|std::this_thread::sleep_until|sync_wait\s*\(|\.wait\s*\(|\.get\s*\('

if [[ -d include/wh ]] && rg -n "$pattern" include/wh; then
  echo "[blocking] FAIL blocking/scheduler-hostile primitive found"
  status=1
else
  echo "[blocking] PASS"
fi

exit $status
