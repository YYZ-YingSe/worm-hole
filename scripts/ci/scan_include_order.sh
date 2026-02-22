#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

status=0

file_listing="$(git ls-files 'include/wh/**/*.hpp' 'include/wh/**/*.h' 'src/**/*.cpp' 'src/**/*.cc' 'src/**/*.cxx' 2>/dev/null || true)"
if [[ -z "$file_listing" ]]; then
  echo "[include-order] SKIP no source files"
  exit 0
fi

while IFS= read -r file; do
  [[ -n "$file" ]] || continue
  [[ -f "$file" ]] || continue

  order_state=0
  line_no=0
  while IFS= read -r line; do
    line_no=$((line_no + 1))
    [[ "$line" =~ ^[[:space:]]*#include[[:space:]]+ ]] || continue

    if [[ "$line" =~ ^[[:space:]]*#include[[:space:]]+< ]]; then
      if [[ "$line" =~ (rapidjson|boost|stdexec) ]]; then
        include_class=2
      else
        include_class=1
      fi
    elif [[ "$line" =~ ^[[:space:]]*#include[[:space:]]+\" ]]; then
      if [[ "$line" =~ (thirdy_party|rapidjson|boost|stdexec) ]]; then
        include_class=2
      else
        include_class=3
      fi
    else
      continue
    fi

    if (( include_class < order_state )); then
      echo "[include-order] FAIL $file:$line_no include order should be std -> thirdy_party -> project"
      status=1
      break
    fi
    order_state=$include_class
  done < "$file"
done <<< "$file_listing"

if [[ $status -eq 0 ]]; then
  echo "[include-order] PASS"
fi

exit $status
