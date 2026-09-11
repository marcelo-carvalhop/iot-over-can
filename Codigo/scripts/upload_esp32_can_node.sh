#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "Uso: $0 <NODE_ID 0..31> [UPLOAD_PORT]"
  echo "Exemplos:"
  echo "  $0 1 /dev/ttyUSB0"
  echo "  $0 2"
  exit 1
fi

NODE_ID="$1"
if ! [[ "$NODE_ID" =~ ^[0-9]+$ ]] || [ "$NODE_ID" -lt 0 ] || [ "$NODE_ID" -gt 31 ]; then
  echo "ERRO: NODE_ID deve estar entre 0 e 31." >&2
  exit 2
fi
UPLOAD_PORT="${2:-}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FW_DIR="$REPO_ROOT/Codigo/node-can"

cd "$FW_DIR"

if [ -n "$UPLOAD_PORT" ]; then
  IOT_NODE_ID="$NODE_ID" pio run -t upload --upload-port "$UPLOAD_PORT"
else
  IOT_NODE_ID="$NODE_ID" pio run -t upload
fi
