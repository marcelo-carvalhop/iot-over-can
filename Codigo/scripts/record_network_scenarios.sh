#!/usr/bin/env bash
# Grava a saída da Probe 00 em cada cenário da bancada virtual
# (Codigo/node-can/test/host/sim_network.cpp).
# Uso: record_network_scenarios.sh [diretório]   (padrão: Front/replays)
#
# Cada arquivo traz as linhas que a Probe 00 enviaria à TUI, precedidas do
# instante simulado ("@12345 [GW] ..."). A TUI as reproduz com
#   python -m pico_tui --replay Front/replays/<cenário>.log
# e os testes da TUI as usam como entrada.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${1:-$REPO_ROOT/Front/replays}"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/iot-over-can-record.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT

"$REPO_ROOT/Codigo/scripts/build_network_sim.sh" "$BUILD_DIR/sim_network"
mkdir -p "$OUT_DIR"
while IFS=$'\t' read -r scenario _description; do
  "$BUILD_DIR/sim_network" "$scenario" > "$OUT_DIR/$scenario.log" 2> /dev/null
  echo "gravado: $OUT_DIR/$scenario.log"
done < <("$BUILD_DIR/sim_network" --list)
