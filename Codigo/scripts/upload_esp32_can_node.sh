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

# A chave mestra do enlace wireless vem de .env.local e só é necessária nos
# Nodes funcionais; a Probe 00 não participa do plano de dados com sensores.
LOCAL_ENV="$REPO_ROOT/.env.local"
IOT_LINK_MASTER_KEY=""
if [ "$NODE_ID" -ne 0 ] && [ -f "$LOCAL_ENV" ]; then
  IOT_LINK_MASTER_KEY="$(sed -n 's/^EDGE_LINK_MASTER_KEY=//p' "$LOCAL_ENV" | head -n 1)"
fi
export IOT_LINK_MASTER_KEY
if [ "$NODE_ID" -ne 0 ] && [ -z "$IOT_LINK_MASTER_KEY" ]; then
  echo "AVISO: chave de enlace ausente em .env.local; o plano de dados wireless ficará desabilitado." >&2
fi

cd "$FW_DIR"

if [ -n "$UPLOAD_PORT" ]; then
  IOT_NODE_ID="$NODE_ID" pio run -t upload --upload-port "$UPLOAD_PORT"
else
  IOT_NODE_ID="$NODE_ID" pio run -t upload
fi
