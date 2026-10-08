#!/usr/bin/env bash
# Compila a simulação de bancada virtual do plano de dados wireless
# (Codigo/node-can/test/host). Uso: build_wireless_link_sim.sh <executável>
set -euo pipefail
if [ $# -ne 1 ]; then
  echo "Uso: $0 <caminho-do-executável>" >&2
  exit 2
fi
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINK="$REPO_ROOT/Codigo/common/ioc_link"
NODE="$REPO_ROOT/Codigo/node-can"
HOST="$NODE/test/host"
OUT="$1"
OBJ_DIR="$(mktemp -d "${TMPDIR:-/tmp}/iot-over-can-sim.XXXXXX")"
trap 'rm -rf "$OBJ_DIR"' EXIT

# Chave fixa de teste; não é usada em nenhum equipamento.
TEST_KEY='"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"'

for src in ioc_sha256 ioc_link ioc_wdata ioc_canbits; do
  cc -std=c11 -Wall -Wextra -Werror -I"$LINK" -c "$LINK/$src.c" -o "$OBJ_DIR/$src.o"
done
# stubs_single/node_config.h torna NODE_ID uma variável: o mesmo executável
# atua como Node 1 e como Probe 00.
for src in "$HOST/sim_wireless_link.cpp" "$NODE/src/wireless_link.cpp" "$NODE/src/net_metrics.cpp"; do
  c++ -std=c++17 -Wall -Wextra -Werror \
    -DIOT_LINK_MASTER_KEY_HEX="$TEST_KEY" \
    -I"$HOST/stubs_single" -I"$HOST/stubs" -I"$NODE/include" -I"$LINK" \
    -c "$src" -o "$OBJ_DIR/$(basename "$src").o"
done
c++ "$OBJ_DIR"/*.o -lm -o "$OUT"
