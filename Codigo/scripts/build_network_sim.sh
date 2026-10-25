#!/usr/bin/env bash
# Bancada nativa com pools de instancias gerados na compilacao.
# IOC_SIM_MAX_NODES=6 e IOC_SIM_GENERATIONS=2 por padrao.
# Os 17 cenarios de regressao continuam usando Probe + Nodes 1,2,3.
set -euo pipefail
if [[ $# -ne 1 ]]; then
  echo 'Uso: build_network_sim.sh <executavel>' >&2
  exit 2
fi
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINK="$ROOT/Codigo/common/ioc_link"
PANEL="$ROOT/Codigo/common/ioc_panel"
PAPER="$ROOT/Codigo/node-paper/src"
NODE="$ROOT/Codigo/node-can"
HOST="$NODE/test/host"
OUT="$1"
OBJ_DIR="$(mktemp -d "${TMPDIR:-/tmp}/iot-over-can-net.XXXXXX")"
trap 'rm -rf "$OBJ_DIR"' EXIT

# Esta chave e dedicada ao ambiente de simulacao e nao pode ser provisionada em hardware.
TEST_KEY='"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"'
CXXFLAGS=(-std=c++17 -O1 -Wall -Wextra -Werror -DIOT_LINK_MASTER_KEY_HEX="$TEST_KEY"
          -I"$HOST" -I"$HOST/stubs" -I"$NODE/include" -I"$LINK" -I"$PANEL" -I"$PAPER" -I"$OBJ_DIR")
SOURCES=(main.ino comandos.cpp falhas.cpp wireless_discovery.cpp wireless_link.cpp
         net_metrics.cpp fault_injection.cpp)

python3 "$ROOT/Simulacao/gen_sim_factories.py" \
  --max-nodes "${IOC_SIM_MAX_NODES:-6}" \
  --generations "${IOC_SIM_GENERATIONS:-2}" \
  --out "$OBJ_DIR" > "$OBJ_DIR/instances.txt"
mapfile -t INSTANCES < "$OBJ_DIR/instances.txt"

# Limitar a concorrencia evita consumir toda a RAM em machines com muitos nucleos.
JOBS="${IOC_SIM_BUILD_JOBS:-4}"
if ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo 'IOC_SIM_BUILD_JOBS deve ser inteiro positivo' >&2
  exit 2
fi
pids=()
failed=0
run() {
  "$@" &
  pids+=("$!")
  if [[ ${#pids[@]} -ge $JOBS ]]; then
    wait "${pids[0]}" || failed=1
    pids=("${pids[@]:1}")
  fi
}

for src in ioc_sha256 ioc_link ioc_wdata ioc_canbits ioc_failover ioc_bindstore; do
  run cc -std=c11 -O1 -Wall -Wextra -Werror -I"$LINK" -c "$LINK/$src.c" -o "$OBJ_DIR/$src.o"
done
for src in ioc_panel_proto ioc_panel_cache ioc_view ioc_page; do
  run cc -std=c11 -O1 -Wall -Wextra -Werror -I"$LINK" -I"$PANEL" -c "$PANEL/$src.c" -o "$OBJ_DIR/$src.o"
done
run c++ "${CXXFLAGS[@]}" -c "$HOST/sim_network.cpp" -o "$OBJ_DIR/sim_network.o"
# Lógica do PaperColor (a mesma do firmware), para o painel virtual da bancada.
run c++ "${CXXFLAGS[@]}" -c "$PAPER/panel_app.cpp" -o "$OBJ_DIR/panel_app.o"
for entry in "${INSTANCES[@]}"; do
  ns="${entry%%:*}"
  id="${entry##*:}"
  run c++ "${CXXFLAGS[@]}" -DSIM_NS="sim_$ns" -DSIM_FACTORY="sim_make_$ns" -DIOT_NODE_ID="$id" \
    -c "$HOST/sim_instance.cpp" -o "$OBJ_DIR/$ns-instance.o"
  for src in "${SOURCES[@]}"; do
    run c++ "${CXXFLAGS[@]}" -DSIM_NS="sim_$ns" -DIOT_NODE_ID="$id" \
      -DSIM_SOURCE="\"$NODE/src/$src\"" \
      -c "$HOST/sim_unit.cpp" -o "$OBJ_DIR/$ns-$src.o"
  done
done
for pid in "${pids[@]}"; do
  wait "$pid" || failed=1
done
if [[ "$failed" != 0 ]]; then
  echo 'build_network_sim: compilacao falhou' >&2
  exit 1
fi
c++ "$OBJ_DIR"/*.o -lm -o "$OUT"
echo "build_network_sim: criado $OUT (${#INSTANCES[@]} instancias compiladas)" >&2
