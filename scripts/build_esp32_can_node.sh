#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ] && [ -z "${IOT_NODE_ID:-}" ]; then
  echo "Uso: $0 <NODE_ID 0..31>" >&2
  echo "Exemplo: $0 2" >&2
  exit 1
fi

NODE_ID="${1:-${IOT_NODE_ID}}"
if ! [[ "$NODE_ID" =~ ^[0-9]+$ ]] || [ "$NODE_ID" -lt 0 ] || [ "$NODE_ID" -gt 31 ]; then
  echo "ERRO: NODE_ID deve estar entre 0 e 31." >&2
  exit 2
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW_DIR="$REPO_ROOT/Codigo/node-can"

cd "$FW_DIR"
IOT_NODE_ID="$NODE_ID" pio run
