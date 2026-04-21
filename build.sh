#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN="${PYTHON_BIN:-}"

if [[ -z "$PYTHON_BIN" ]]; then
  if command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN="python3"
  elif command -v python >/dev/null 2>&1; then
    PYTHON_BIN="python"
  else
    echo "[build.sh] FAIL python3/python not found" >&2
    exit 1
  fi
fi

if [[ $# -eq 0 ]]; then
  exec "$PYTHON_BIN" "$ROOT/scripts/toolchain.py" --help
fi

case "$1" in
  ci|local)
    exec "$PYTHON_BIN" "$ROOT/scripts/toolchain.py" "$@"
    ;;
  configure|build|test|verify|clean|sync-third-party|editor)
    exec "$PYTHON_BIN" "$ROOT/scripts/toolchain.py" local "$@"
    ;;
  help|-h|--help)
    exec "$PYTHON_BIN" "$ROOT/scripts/toolchain.py" --help
    ;;
  *)
    echo "[build.sh] FAIL unsupported command: $1" >&2
    echo "usage: ./build.sh <configure|build|test|verify|clean|sync-third-party|editor|local|ci> [...]" >&2
    exit 2
    ;;
esac
