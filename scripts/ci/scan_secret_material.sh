#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

status=0

# 仓库内禁止跟踪 .ssh 目录
if git ls-files | rg -n '^\.ssh/' >/dev/null 2>&1; then
  echo "[secret-scan] FAIL tracked .ssh material detected"
  git ls-files | rg '^\.ssh/' || true
  status=1
fi

# 私钥头扫描（仅扫描已跟踪文件，排除本扫描脚本）
if git grep -n -I \
  -e '-----BEGIN OPENSSH PRIVATE KEY-----' \
  -e '-----BEGIN RSA PRIVATE KEY-----' \
  -e '-----BEGIN PRIVATE KEY-----' \
  -- ':!scripts/ci/scan_secret_material.sh' >/dev/null 2>&1; then
  echo "[secret-scan] FAIL private key content detected"
  status=1
fi

# 常见私钥文件名扫描（允许 .pub）
if git ls-files | rg -n '(id_rsa|id_ed25519|github_ed25519)$' >/dev/null 2>&1; then
  echo "[secret-scan] FAIL private-key-like filename tracked"
  git ls-files | rg '(id_rsa|id_ed25519|github_ed25519)$' || true
  status=1
fi

if [[ $status -eq 0 ]]; then
  echo "[secret-scan] PASS"
fi

exit $status
