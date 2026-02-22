#!/usr/bin/env bash
set -euo pipefail

status=0
event_name="${GITHUB_EVENT_NAME:-local}"
github_ref="${GITHUB_REF:-}"
head_ref="${GITHUB_HEAD_REF:-}"
base_ref="${GITHUB_BASE_REF:-}"

if [[ "$event_name" == "pull_request" ]]; then
  if [[ -z "$head_ref" || ! "$head_ref" =~ ^feature/ ]]; then
    echo "[branch-policy] FAIL pull_request head branch must be feature/*, got: ${head_ref:-<empty>}"
    status=1
  fi

  if [[ -n "$base_ref" && "$base_ref" != "main" ]]; then
    echo "[branch-policy] FAIL pull_request base branch must be main, got: $base_ref"
    status=1
  fi
fi

if [[ "$event_name" == "push" && "$github_ref" == "refs/heads/main" ]]; then
  echo "[branch-policy] WARN push to main detected; enforce branch protection in repository settings"
fi

if [[ $status -eq 0 ]]; then
  echo "[branch-policy] PASS"
fi

exit $status
