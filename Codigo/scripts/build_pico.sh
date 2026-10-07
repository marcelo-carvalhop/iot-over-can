#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FW_DIR="$REPO_ROOT/Codigo/node-wifi"
LOCAL_ENV="$REPO_ROOT/.env.local"

usage() {
  echo "Uso: $0 [pico_w|pico2_w] [--sensor <UUID64>]" >&2
  echo "  --sensor  usa .env.sensor-<UUID>.local (chaves derivadas para este sensor)." >&2
}

PICO_BOARD_TARGET="${PICO_BOARD_TARGET:-pico_w}"
SENSOR_UUID=""
while [ $# -gt 0 ]; do
  case "$1" in
    pico_w|pico2_w) PICO_BOARD_TARGET="$1" ;;
    --sensor)
      SENSOR_UUID="${2:-}"
      [ -n "$SENSOR_UUID" ] || { usage; exit 2; }
      shift
      ;;
    *) usage; exit 2 ;;
  esac
  shift
done

if [ "$PICO_BOARD_TARGET" != "pico_w" ] && [ "$PICO_BOARD_TARGET" != "pico2_w" ]; then
  usage
  exit 2
fi

# Lê NOME=valor sem executar o arquivo de segredos.
read_env_value() {
  local file="$1" name="$2"
  [ -f "$file" ] || return 0
  sed -n "s/^${name}=//p" "$file" | head -n 1
}

EDGE_LINK_DEVICE_KEY=""
EDGE_LINK_NETWORK_KEY=""
EDGE_LINK_DEVICE_UUID=""
EDGE_LINK_MASTER_KEY=""
LINK_MODE="sem chave de enlace: o sensor anuncia por BLE, mas não abre sessão com Nodes"

if [ -n "$SENSOR_UUID" ]; then
  SENSOR_UUID="$(printf '%s' "${SENSOR_UUID#0x}" | tr '[:lower:]' '[:upper:]')"
  SENSOR_ENV="$REPO_ROOT/.env.sensor-$SENSOR_UUID.local"
  if [ ! -f "$SENSOR_ENV" ]; then
    echo "ERRO: $SENSOR_ENV não existe." >&2
    echo "Gere-o com ./Codigo/scripts/provision_sensor_security.sh --sensor $SENSOR_UUID" >&2
    exit 3
  fi
  EDGE_SERIAL_ADMIN_TOKEN="$(read_env_value "$SENSOR_ENV" EDGE_SERIAL_ADMIN_TOKEN)"
  EDGE_LINK_DEVICE_KEY="$(read_env_value "$SENSOR_ENV" EDGE_LINK_DEVICE_KEY)"
  EDGE_LINK_NETWORK_KEY="$(read_env_value "$SENSOR_ENV" EDGE_LINK_NETWORK_KEY)"
  EDGE_LINK_DEVICE_UUID="$(read_env_value "$SENSOR_ENV" EDGE_LINK_DEVICE_UUID)"
  LINK_MODE="chaves derivadas para o sensor 0x$SENSOR_UUID"
else
  EDGE_SERIAL_ADMIN_TOKEN="$(read_env_value "$LOCAL_ENV" EDGE_SERIAL_ADMIN_TOKEN)"
  EDGE_LINK_MASTER_KEY="$(read_env_value "$LOCAL_ENV" EDGE_LINK_MASTER_KEY)"
  if [ -n "$EDGE_LINK_MASTER_KEY" ]; then
    LINK_MODE="chave mestra (bancada); para uso permanente compile com --sensor <UUID>"
  fi
fi
EDGE_SERIAL_ADMIN_TOKEN="${EDGE_SERIAL_ADMIN_TOKEN:-0}"

echo "Compilando node-wifi para PICO_BOARD=$PICO_BOARD_TARGET"
echo "Enlace: $LINK_MODE"

cd "$FW_DIR"
rm -rf build
mkdir -p build
cd build
cmake \
  -DPICO_BOARD="$PICO_BOARD_TARGET" \
  -DEDGE_SERIAL_ADMIN_TOKEN="$EDGE_SERIAL_ADMIN_TOKEN" \
  -DEDGE_LINK_DEVICE_KEY="$EDGE_LINK_DEVICE_KEY" \
  -DEDGE_LINK_NETWORK_KEY="$EDGE_LINK_NETWORK_KEY" \
  -DEDGE_LINK_DEVICE_UUID="$EDGE_LINK_DEVICE_UUID" \
  -DEDGE_LINK_MASTER_KEY="$EDGE_LINK_MASTER_KEY" \
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
if [ -z "$EDGE_LINK_DEVICE_KEY" ] && [ -z "$EDGE_LINK_MASTER_KEY" ]; then
  echo "AVISO: chave de enlace não provisionada; o plano de dados via Node CAN ficará desabilitado."
fi
