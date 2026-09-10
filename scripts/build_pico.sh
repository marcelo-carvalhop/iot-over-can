#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW_DIR="$REPO_ROOT/Codigo/node-wifi"
LOCAL_ENV="$REPO_ROOT/.env.local"

if [ -f "$LOCAL_ENV" ]; then
  # shellcheck disable=SC1090
  source "$LOCAL_ENV"
fi

EDGE_SERIAL_ADMIN_TOKEN="${EDGE_SERIAL_ADMIN_TOKEN:-0}"
EDGE_NODE_PRESHARED_KEY="${EDGE_NODE_PRESHARED_KEY:-0}"
EDGE_ALLOW_LEGACY_INSECURE_UDP_CONTROL="${EDGE_ALLOW_LEGACY_INSECURE_UDP_CONTROL:-0}"

cd "$FW_DIR"
rm -rf build
mkdir -p build
cd build
cmake \
  -DPICO_BOARD=pico_w \
  -DEDGE_SERIAL_ADMIN_TOKEN="$EDGE_SERIAL_ADMIN_TOKEN" \
  -DEDGE_NODE_PRESHARED_KEY="$EDGE_NODE_PRESHARED_KEY" \
  -DEDGE_ALLOW_LEGACY_INSECURE_UDP_CONTROL="$EDGE_ALLOW_LEGACY_INSECURE_UDP_CONTROL" \
  ..
cmake --build . -j"$(nproc)"
echo
echo "Build finalizado. Arquivos gerados em: $FW_DIR/build"
if [ "$EDGE_SERIAL_ADMIN_TOKEN" = "0" ]; then
  echo "AVISO: token serial não provisionado; comandos mutáveis via USB permanecerão bloqueados."
fi
