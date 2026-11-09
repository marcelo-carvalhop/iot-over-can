#!/usr/bin/env bash
# Compila (e, com --upload <porta>, grava) o firmware do painel (ESP32-2432S028, "CYD").
#   build_paper.sh                       bancada: chave mestra de .env.local, UUID pelo MAC
#   build_paper.sh --sensor <UUID64>     chaves derivadas de .env.sensor-<UUID>.local
#   build_paper.sh --upload /dev/ttyUSB0 grava depois de compilar
# --cyd continua aceito (é o único painel). O leitor da documentação no
# M5Stack PaperColor tem o próprio script: build_leitor.sh.
# Sem chave nenhuma o painel ainda funciona pela USB com o Simulation Studio.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FW_DIR="$REPO_ROOT/Codigo/node-paper"
UUID=""
PORT=""
PIO_ENV=(-e cyd)
while [ $# -gt 0 ]; do
  case "$1" in
    --sensor) UUID="${2:-}"; shift 2 ;;
    --upload) PORT="${2:-}"; shift 2 ;;
    --cyd) shift ;;
    *) echo "Uso: $0 [--cyd] [--sensor <UUID64>] [--upload <porta>]" >&2; exit 2 ;;
  esac
done
unset IOT_PANEL_MASTER_KEY IOT_PANEL_DEVICE_KEY IOT_PANEL_NETWORK_KEY IOT_PANEL_UUID
if [ -n "$UUID" ]; then
  ENV_FILE="$REPO_ROOT/.env.sensor-$UUID.local"
  [ -f "$ENV_FILE" ] || { echo "ERRO: $ENV_FILE ausente. Gere com provision_sensor_security.sh --sensor $UUID" >&2; exit 2; }
  export IOT_PANEL_DEVICE_KEY="$(sed -n 's/^EDGE_LINK_DEVICE_KEY=//p' "$ENV_FILE" | head -n 1)"
  export IOT_PANEL_NETWORK_KEY="$(sed -n 's/^EDGE_LINK_NETWORK_KEY=//p' "$ENV_FILE" | head -n 1)"
  export IOT_PANEL_UUID="$UUID"
elif [ -f "$REPO_ROOT/.env.local" ]; then
  export IOT_PANEL_MASTER_KEY="$(sed -n 's/^EDGE_LINK_MASTER_KEY=//p' "$REPO_ROOT/.env.local" | head -n 1)"
fi
cd "$FW_DIR"
if [ -n "$PORT" ]; then
  pio run ${PIO_ENV[@]+"${PIO_ENV[@]}"} -t upload --upload-port "$PORT"
else
  pio run ${PIO_ENV[@]+"${PIO_ENV[@]}"}
fi
