#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

if [[ $# -ne 1 ]]; then
  echo "[compile-commands] FAIL usage: $0 <build_dir>"
  exit 2
fi

build_dir="$1"
source_file="${build_dir%/}/compile_commands.json"
target_file="$ROOT/compile_commands.json"

if [[ ! -f "$source_file" ]]; then
  echo "[compile-commands] SKIP missing $source_file"
  exit 0
fi

cp "$source_file" "$target_file"
echo "[compile-commands] SYNC $source_file -> $target_file"
