#!/usr/bin/env bash
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FW="$REPO_ROOT/Codigo/node-wifi"
LINK="$REPO_ROOT/Codigo/common/ioc_link"
NODE="$REPO_ROOT/Codigo/node-can"
OUT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/iot-over-can-native.XXXXXX")"
trap 'rm -rf "$OUT_DIR"' EXIT

# 1. Validação de configuração do sensor.
cc -std=c11 -Wall -Wextra -Werror \
  -I"$FW" \
  "$FW/config_validation.c" \
  "$FW/tests/test_config_validation.c" \
  -lm -o "$OUT_DIR/config_validation"
"$OUT_DIR/config_validation"

# 2. Biblioteca compartilhada: SHA-256/HMAC, autenticação do enlace e
#    segmentação do plano de dados no CAN.
cc -std=c11 -Wall -Wextra -Werror \
  -I"$LINK" \
  "$LINK/ioc_sha256.c" "$LINK/ioc_link.c" "$LINK/ioc_wdata.c" \
  "$REPO_ROOT/Codigo/common/tests/test_ioc_link.c" \
  -lm -o "$OUT_DIR/test_ioc_link"
"$OUT_DIR/test_ioc_link"

# 3. Simulação sensor <-> Node <-> Probe com o código real de wireless_link.cpp.
"$REPO_ROOT/Codigo/scripts/build_wireless_link_sim.sh" "$OUT_DIR/sim_wireless_link"
"$OUT_DIR/sim_wireless_link" > /dev/null

echo "native firmware validation tests: PASS"
