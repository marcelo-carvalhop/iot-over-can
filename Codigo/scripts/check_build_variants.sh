#!/usr/bin/env bash
# Confere que as opções de compilação documentadas do Módulo CAN compilam: a bancada virtual só roda com os
# padrões, e uma opção que nunca é ligada acumula erros sem ninguém ver (a detecção de isolamento desligada,
# por exemplo, falhava com -Werror=type-limits). Cada variante compila só os arquivos que a opção toca,
# no mesmo esquema da bancada (sim_unit.cpp), apenas até a verificação de sintaxe.
#
# Uso: check_build_variants.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINK="$ROOT/Codigo/common/ioc_link"
PANEL="$ROOT/Codigo/common/ioc_panel"
PAPER="$ROOT/Codigo/node-paper/src"
NODE="$ROOT/Codigo/node-can"
HOST="$NODE/test/host"
OBJ_DIR="$(mktemp -d "${TMPDIR:-/tmp}/iot-over-can-variants.XXXXXX")"
trap 'rm -rf "$OBJ_DIR"' EXIT

TEST_KEY='"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"'
BASE=(-std=c++17 -fsyntax-only -Wall -Wextra -Werror -DIOT_LINK_MASTER_KEY_HEX="$TEST_KEY"
      -I"$HOST" -I"$HOST/stubs" -I"$NODE/include" -I"$LINK" -I"$PANEL" -I"$PAPER" -I"$OBJ_DIR")

# nome | opções | arquivos | identificador do módulo (opcional; 1 por padrão, 0 é a Probe)
VARIANTS=(
  "medicao-da-alimentacao|-DIOT_VIN_ADC_PIN=34 -DIOT_VIN_R1_OHM=200000 -DIOT_VIN_R2_OHM=100000 -DIOT_VIN_WARN_MV=4800 -DIOT_VIN_CRIT_MV=4600 -DIOT_INA219_ADDR=64 -DIOT_INA219_SHUNT_MOHM=100 -DIOT_I2C_SDA=21 -DIOT_I2C_SCL=22|energia.cpp main.ino"
  "so-tensao-de-entrada|-DIOT_VIN_ADC_PIN=35|energia.cpp"
  "so-corrente|-DIOT_INA219_ADDR=65|energia.cpp"
  "isolamento-desligado|-DIOT_ISOLATION_MS=0|isolamento.cpp main.ino"
  "isolamento-ajustado|-DIOT_ISOLATION_MS=12000 -DIOT_ISOLATION_RECOVER_MS=5000|isolamento.cpp"
  "esperas-do-sensor|-DIOT_SESSION_TIMEOUT_MS=7000 -DIOT_OFFER_SWITCH_HOLD_MS=1000|wireless_link.cpp pesquisa.cpp"
  "vigilancia-do-lider-desligada|-DIOT_FOLLOWER_LEADER_WATCHDOG=0|main.ino"
  "sem-injecao-de-falhas|-DIOT_FAULT_INJECTION=0|fault_injection.cpp falhas.cpp comandos.cpp autoteste.cpp isolamento.cpp wireless_link.cpp main.ino"
  "potencia-maxima-do-wifi|-DIOT_WIFI_TX_POWER_QDBM=78|energia.cpp wireless_link.cpp"
  "autenticacao-de-origem|-DIOT_CAN_AUTH=1|canauth.cpp wireless_link.cpp comandos.cpp main.ino|1"
  "autenticacao-de-origem-na-probe|-DIOT_CAN_AUTH=1|canauth.cpp wireless_link.cpp main.ino|0"
)

# O Probe 00 e os Nodes compartilham o código; a variante vale para um Node qualquer, salvo quando o quarto campo diz outro.
failed=0
for entry in "${VARIANTS[@]}"; do
  IFS='|' read -r name flags files node_id <<< "$entry"
  node_id="${node_id:-1}"
  variant_failed=0
  for src in $files; do
    # shellcheck disable=SC2086
    if ! c++ "${BASE[@]}" $flags -DSIM_NS=sim_variante -DIOT_NODE_ID="$node_id" \
        -DSIM_SOURCE="\"$NODE/src/$src\"" "$HOST/sim_unit.cpp" 2> "$OBJ_DIR/erro.txt"; then
      echo "FALHA: variante '$name' ($flags): $src nao compila" >&2
      head -20 "$OBJ_DIR/erro.txt" >&2
      variant_failed=1
      failed=1
    fi
  done
  echo "variante $name: $([ "$variant_failed" = 0 ] && echo ok || echo FALHOU)"
done

# A autenticação ligada sem a chave de enlace não pode compilar: um firmware assim aceitaria ou recusaria tudo
# sem que ninguém percebesse. (BASE traz a chave; aqui ela fica de fora.)
NOKEY=()
for flag in "${BASE[@]}"; do
  case "$flag" in -DIOT_LINK_MASTER_KEY_HEX=*) ;; *) NOKEY+=("$flag") ;; esac
done
for id in 0 1; do
  if c++ "${NOKEY[@]}" -DIOT_CAN_AUTH=1 -DSIM_NS=sim_variante -DIOT_NODE_ID="$id" \
      -DSIM_SOURCE="\"$NODE/src/canauth.cpp\"" "$HOST/sim_unit.cpp" 2> "$OBJ_DIR/erro.txt"; then
    echo "FALHA: IOT_CAN_AUTH=1 sem chave de enlace compilou (modulo $id)" >&2
    failed=1
  elif ! grep -q "IOT_LINK_MASTER_KEY_HEX" "$OBJ_DIR/erro.txt"; then
    echo "FALHA: IOT_CAN_AUTH=1 sem chave falhou por outro motivo (modulo $id):" >&2
    head -10 "$OBJ_DIR/erro.txt" >&2
    failed=1
  fi
done
echo "variante autenticacao-sem-chave-recusada: $([ "$failed" = 0 ] && echo ok || echo FALHOU)"

[ "$failed" = 0 ] || exit 1
echo "check_build_variants: ${#VARIANTS[@]} variantes compilam e a autenticacao sem chave e recusada"
