#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FW_DIR="$REPO_ROOT/Codigo/node-wifi"
LOCAL_ENV="$REPO_ROOT/.env.local"

if [ -f "$LOCAL_ENV" ]; then
  # shellcheck disable=SC1090
  source "$LOCAL_ENV"
fi

EDGE_SERIAL_ADMIN_TOKEN="${EDGE_SERIAL_ADMIN_TOKEN:-0}"
EDGE_NODE_PRESHARED_KEY="${EDGE_NODE_PRESHARED_KEY:-0}"
EDGE_ALLOW_LEGACY_INSECURE_UDP_CONTROL="${EDGE_ALLOW_LEGACY_INSECURE_UDP_CONTROL:-0}"
PICO_BOARD_TARGET="${1:-${PICO_BOARD_TARGET:-pico_w}}"

if [ "$PICO_BOARD_TARGET" != "pico_w" ] && [ "$PICO_BOARD_TARGET" != "pico2_w" ]; then
  echo "Uso: $0 [pico_w|pico2_w]" >&2
  exit 2
fi
echo "Compilando node-wifi para PICO_BOARD=$PICO_BOARD_TARGET"

cd "$FW_DIR"
rm -rf build
mkdir -p build
cd build
cmake \
  -DPICO_BOARD="$PICO_BOARD_TARGET" \
  -DEDGE_SERIAL_ADMIN_TOKEN="$EDGE_SERIAL_ADMIN_TOKEN" \
  -DEDGE_NODE_PRESHARED_KEY="$EDGE_NODE_PRESHARED_KEY" \
  -DEDGE_ALLOW_LEGACY_INSECURE_UDP_CONTROL="$EDGE_ALLOW_LEGACY_INSECURE_UDP_CONTROL" \
  ..
cmake --build . -j"$(nproc)"

UF2="$FW_DIR/build/edge_node_firmware.uf2"
if [ ! -f "$UF2" ]; then
  echo
  echo "ERRO: a compilação terminou sem gerar o artefato esperado:"
  echo "  $UF2"
  exit 1
fi

echo
echo "Build finalizado com sucesso."
echo "UF2: $UF2"
ls -lh "$UF2"

if [ "$EDGE_SERIAL_ADMIN_TOKEN" = "0" ]; then
  echo "AVISO: token serial não provisionado; comandos mutáveis via USB permanecerão bloqueados."
fi
