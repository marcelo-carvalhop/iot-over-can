#!/usr/bin/env bash
# Compila a bancada virtual (Codigo/node-can/test/host/sim_network.cpp): a
# Probe 00, três Nodes CAN e sensores simulados em um único executável.
# Uso: build_network_sim.sh <executável>
#
# Cada Node é o firmware real compilado em um namespace próprio. Os Nodes 1 a
# 3 têm duas instâncias cada, porque um reinício consome uma instância.
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
OBJ_DIR="$(mktemp -d "${TMPDIR:-/tmp}/iot-over-can-net.XXXXXX")"
trap 'rm -rf "$OBJ_DIR"' EXIT

# Chave fixa de teste; não é usada em nenhum equipamento.
TEST_KEY='"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"'
CXXFLAGS=(-std=c++17 -O1 -Wall -Wextra -Werror -DIOT_LINK_MASTER_KEY_HEX="$TEST_KEY"
          -I"$HOST" -I"$HOST/stubs" -I"$NODE/include" -I"$LINK")
SOURCES=(main.ino comandos.cpp falhas.cpp wireless_discovery.cpp wireless_link.cpp
         net_metrics.cpp fault_injection.cpp)
# namespace:NODE_ID
INSTANCES=(probe:0 n1a:1 n1b:1 n2a:2 n2b:2 n3a:3 n3b:3)

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
pids=()
failed=0

# Executa o comando em segundo plano, limitando a JOBS compilações simultâneas.
run() {
  "$@" &
  pids+=("$!")
  if [ "${#pids[@]}" -ge "$JOBS" ]; then
    wait "${pids[0]}" || failed=1
    pids=("${pids[@]:1}")
  fi
}

for src in ioc_sha256 ioc_link ioc_wdata ioc_canbits ioc_failover; do
  run cc -std=c11 -O1 -Wall -Wextra -Werror -I"$LINK" -c "$LINK/$src.c" -o "$OBJ_DIR/$src.o"
done
run c++ "${CXXFLAGS[@]}" -c "$HOST/sim_network.cpp" -o "$OBJ_DIR/sim_network.o"
for instance in "${INSTANCES[@]}"; do
  ns="${instance%%:*}"
  id="${instance##*:}"
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
if [ "$failed" -ne 0 ]; then
  echo "build_network_sim: compilação falhou" >&2
  exit 1
fi

c++ "$OBJ_DIR"/*.o -lm -o "$OUT"
