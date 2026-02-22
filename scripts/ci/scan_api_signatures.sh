#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

status=0

# 公开接口禁止裸 timeout_ms
if rg -n "\b(int|long|size_t)\s+timeout_ms\b" include/wh 2>/dev/null; then
  echo "[api-signature] FAIL raw timeout_ms in public headers"
  status=1
fi

# 公开接口禁止魔法 bool 参数（基础启发式）
if rg -n "\b[a-zA-Z_][a-zA-Z0-9_:<>]*\s+[a-zA-Z_][a-zA-Z0-9_]*\s*\([^\)]*\bbool\s+[a-zA-Z_][a-zA-Z0-9_]*" include/wh 2>/dev/null; then
  echo "[api-signature] FAIL bool parameter found in public API; use *_options"
  status=1
fi

if [[ $status -eq 0 ]]; then
  echo "[api-signature] PASS"
fi

exit $status
