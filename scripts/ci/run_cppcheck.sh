#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

strict_mode="${WH_CI_STRICT:-0}"

if ! command -v cppcheck >/dev/null 2>&1; then
  if [[ "$strict_mode" == "1" || -n "${CI:-}" ]]; then
    echo "[cppcheck] FAIL cppcheck not installed in strict mode"
    exit 1
  fi
  echo "[cppcheck] SKIP cppcheck not installed"
  exit 0
fi

if [[ "${RUNNER_OS:-}" == "Windows" ]]; then
  probe_src="$(mktemp "${TMPDIR:-/tmp}/cppcheck-probe-XXXXXX.cpp")"
  printf 'int main() { return 0; }\n' > "$probe_src"

  probe_output=""
  if ! probe_output="$(cppcheck --quiet --error-exitcode=2 --std=c++20 "$probe_src" 2>&1)"; then
    rm -f "$probe_src"

    if printf '%s\n' "$probe_output" | rg -q 'std\.cfg|installation is broken'; then
      echo "[cppcheck] SKIP windows runner cppcheck package missing std.cfg"
      exit 0
    fi

    if [[ "$strict_mode" == "1" || -n "${CI:-}" ]]; then
      echo "[cppcheck] FAIL cppcheck probe failed on windows"
      printf '%s\n' "$probe_output"
      exit 1
    fi

    echo "[cppcheck] SKIP cppcheck probe failed on windows"
    printf '%s\n' "$probe_output"
    exit 0
  fi

  rm -f "$probe_src"
fi

source_listing="$(git ls-files 'include/wh/**/*.hpp' 'include/wh/**/*.h' 'src/**/*.cpp' 'src/**/*.cc' 'src/**/*.cxx' 'tests/**/*.cpp' 'tests/**/*.cc' 'tests/**/*.cxx' 2>/dev/null || true)"
if [[ -z "$source_listing" ]]; then
  echo "[cppcheck] SKIP no source files"
  exit 0
fi

source_files=()
while IFS= read -r path; do
  [[ -n "$path" ]] || continue
  source_files+=("$path")
done <<< "$source_listing"

cppcheck \
  --enable=warning,performance,portability \
  --error-exitcode=2 \
  --language=c++ \
  --std=c++20 \
  --library=posix \
  --quiet \
  "${source_files[@]}"

echo "[cppcheck] PASS"
