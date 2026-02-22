#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

usage() {
  cat <<'EOF'
Usage: tools/thirdy_party_git.sh <command> [args]

Commands:
  sync
    git submodule sync --recursive
    git submodule update --init --recursive

  status
    show submodule commit status and lock summary

  add <name> <repo> <commit>
    add submodule under thirdy_party/<name> and checkout pinned commit

  pin <name> <commit>
    pin existing submodule to commit

  lock
    regenerate thirdy_party/LOCK.txt from current git submodules
EOF
}

ensure_repo_root() {
  if [[ ! -d .git ]]; then
    echo "[thirdy-party-git] must run in repo root" >&2
    exit 1
  fi
}

ensure_sha() {
  local value="$1"
  if [[ ! "$value" =~ ^[0-9a-fA-F]{40}$ ]]; then
    echo "[thirdy-party-git] commit must be 40-char SHA: $value" >&2
    exit 1
  fi
}

write_lock() {
  mkdir -p thirdy_party
  {
    echo "# thirdy_party lock"
    echo "# generated_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "# format=path|remote_url|commit_sha|tree_sha"
    while read -r _sha path; do
      [[ -n "$path" ]] || continue
      local_sha="$(git -C "$path" rev-parse HEAD 2>/dev/null || true)"
      tree_sha="$(git -C "$path" rev-parse HEAD^{tree} 2>/dev/null || true)"
      local_url="$(git -C "$path" config --get remote.origin.url 2>/dev/null || true)"
      echo "$path|$local_url|$local_sha|$tree_sha"
    done < <(git submodule status | awk '{print $1" "$2}')
  } > thirdy_party/LOCK.txt

  echo "[thirdy-party-git] lock updated: thirdy_party/LOCK.txt"
}

cmd="${1:-}"
if [[ -z "$cmd" ]]; then
  usage
  exit 1
fi
shift || true

ensure_repo_root

case "$cmd" in
  sync)
    git submodule sync --recursive
    git submodule update --init --recursive
    write_lock
    ;;
  status)
    git submodule status || true
    if [[ -f thirdy_party/LOCK.txt ]]; then
      echo "--- LOCK ---"
      cat thirdy_party/LOCK.txt
    fi
    ;;
  add)
    name="${1:-}"
    repo="${2:-}"
    commit="${3:-}"
    if [[ -z "$name" || -z "$repo" || -z "$commit" ]]; then
      usage
      exit 1
    fi
    ensure_sha "$commit"
    git submodule add "$repo" "thirdy_party/$name"
    git -C "thirdy_party/$name" fetch --tags --all
    git -C "thirdy_party/$name" checkout "$commit"
    git add .gitmodules "thirdy_party/$name"
    write_lock
    ;;
  pin)
    name="${1:-}"
    commit="${2:-}"
    if [[ -z "$name" || -z "$commit" ]]; then
      usage
      exit 1
    fi
    ensure_sha "$commit"
    git -C "thirdy_party/$name" fetch --tags --all
    git -C "thirdy_party/$name" checkout "$commit"
    git add "thirdy_party/$name"
    write_lock
    ;;
  lock)
    write_lock
    ;;
  *)
    usage
    exit 1
    ;;
esac
