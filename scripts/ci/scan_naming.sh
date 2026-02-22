#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

status=0

# 路径命名：源码和测试路径禁止大写
while IFS= read -r path; do
  if [[ "$path" =~ [A-Z] ]]; then
    echo "[naming] FAIL uppercase path: $path"
    status=1
  fi
done < <(git ls-files | rg '^(include/wh|src|tests)/' || true)

# token 命名：禁止 useCamelCase / use-xxx
if [[ -d include/wh ]]; then
  if rg -n "\buse[A-Z][A-Za-z0-9_]*\b|\buse-[a-z0-9_]+\b" include/wh; then
    echo "[naming] FAIL token naming violated; require use_xxx"
    status=1
  fi
fi

# 禁止过程式命名污染公开 API
if [[ -d include/wh ]]; then
  if rg -n "\b(task[0-9]+|l[0-9]+)\b" include/wh; then
    echo "[naming] FAIL process-style naming detected"
    status=1
  fi
fi

if [[ $status -eq 0 ]]; then
  echo "[naming] PASS"
fi

exit $status
