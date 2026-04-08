#!/usr/bin/env bash
set -euo pipefail

# Public local entrypoint for worm-hole configure/build/test flows.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT/scripts/build/cli.sh"
wh_build_cli_main "$@"
