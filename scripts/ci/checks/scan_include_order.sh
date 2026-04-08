#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"

status=0

class_std=1
class_third_party=2
class_project=3

include_angle_pattern='^[[:space:]]*#include[[:space:]]+<([^>]+)>'
include_quote_pattern='^[[:space:]]*#include[[:space:]]+"([^"]+)"'

classify_angle_include() {
  local target="$1"

  case "$target" in
    stdexec/*|exec/*|rapidjson/*|boost/*|minja/*|nlohmann/*|catch2/*)
      printf '%s\n' "$class_third_party"
      ;;
    *)
      printf '%s\n' "$class_std"
      ;;
  esac
}

classify_quote_include() {
  local target="$1"

  case "$target" in
    wh/*)
      printf '%s\n' "$class_project"
      ;;
    thirdy_party/*|stdexec/*|exec/*|rapidjson/*|boost/*|minja/*|nlohmann/*|catch2/*)
      printf '%s\n' "$class_third_party"
      ;;
    *)
      printf '%s\n' "$class_project"
      ;;
  esac
}

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

    include_target=""
    if [[ "$line" =~ $include_angle_pattern ]]; then
      include_target="${BASH_REMATCH[1]}"
      include_class="$(classify_angle_include "$include_target")"
    elif [[ "$line" =~ $include_quote_pattern ]]; then
      include_target="${BASH_REMATCH[1]}"
      include_class="$(classify_quote_include "$include_target")"
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
