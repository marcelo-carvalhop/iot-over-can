#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FW_DIR="$REPO_ROOT/Codigo/node-wifi"
FW_NAME="node-wifi"
UF2_NAME="edge_node_firmware.uf2"
LOCAL_ENV="$REPO_ROOT/.env.local"

usage() {
  echo "Uso: $0 [pico_w|pico2_w] [--audio] [--sensor <UUID64>] [--mic-pins <SCK>,<SD>] [--sem-chave]" >&2
  echo "       [--espera-sessao <ms>] [--espera-oferta <ms>]" >&2
  echo "  --audio     compila o sensor acústico INMP441 (Codigo/node-audio) em vez do de vibração." >&2
  echo "  --sensor    usa .env.sensor-<UUID>.local (chaves derivadas para este sensor)." >&2
  echo "  --mic-pins  GPIO do SCK e do SD do INMP441 (WS = SCK + 1); padrão 18,20. Só com --audio." >&2
  echo "  --sem-chave compila mesmo sem chave de enlace (o sensor anuncia por BLE, mas não abre sessão)." >&2
  echo "  --espera-sessao  tempo sem CMD_PING antes de o sensor largar a sessão, de 7000 a 60000 ms (padrão 10000)." >&2
  echo "  --espera-oferta  tempo antes de o sensor seguir a oferta de outro módulo, de 1000 a 120000 ms (padrão 15000)." >&2
  echo "                   Os dois só devem mudar com medição em bancada física (Documentacao/validacao.md)." >&2
  echo "A saída fica em <firmware>/build/<placa>[-<UUID>][-mic<SCK>-<SD>][-sess<ms>][-oferta<ms>]/, uma pasta por variante:" >&2
  echo "compilar para outra placa ou para outro sensor não apaga o .uf2 anterior." >&2
}

PICO_BOARD_TARGET="${PICO_BOARD_TARGET:-pico_w}"
SENSOR_UUID=""
MIC_PINS=""
WAIT_SESSION=""
WAIT_OFFER=""
ALLOW_NO_KEY=0
AUDIO=0
while [ $# -gt 0 ]; do
  case "$1" in
    pico_w|pico2_w) PICO_BOARD_TARGET="$1" ;;
    --audio)
      AUDIO=1
      FW_DIR="$REPO_ROOT/Codigo/node-audio"
      FW_NAME="node-audio"
      UF2_NAME="edge_audio_firmware.uf2"
      ;;
    --sensor)
      SENSOR_UUID="${2:-}"
      [ -n "$SENSOR_UUID" ] || { usage; exit 2; }
      shift
      ;;
    --mic-pins)
      MIC_PINS="${2:-}"
      [ -n "$MIC_PINS" ] || { usage; exit 2; }
      shift
      ;;
    --espera-sessao)
      WAIT_SESSION="${2:-}"
      [ -n "$WAIT_SESSION" ] || { usage; exit 2; }
      shift
      ;;
    --espera-oferta)
      WAIT_OFFER="${2:-}"
      [ -n "$WAIT_OFFER" ] || { usage; exit 2; }
      shift
      ;;
    --sem-chave) ALLOW_NO_KEY=1 ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
  shift
done

# Pinos do microfone: SCK e SD; o WS é sempre SCK + 1 (exigência do side-set da PIO). Os GPIO 23, 24, 25 e
# 29 do Pico W são do rádio e da medição da bateria.
AUDIO_PIN_SCK=""
AUDIO_PIN_SD=""
if [ -n "$MIC_PINS" ]; then
  [ "$AUDIO" = 1 ] || { echo "ERRO: --mic-pins só vale com --audio." >&2; exit 2; }
  if ! [[ "$MIC_PINS" =~ ^([0-9]+),([0-9]+)$ ]]; then
    echo "ERRO: --mic-pins espera <SCK>,<SD>, por exemplo 18,20." >&2
    exit 2
  fi
  AUDIO_PIN_SCK="${BASH_REMATCH[1]}"
  AUDIO_PIN_SD="${BASH_REMATCH[2]}"
  for pin in "$AUDIO_PIN_SCK" "$((AUDIO_PIN_SCK + 1))" "$AUDIO_PIN_SD"; do
    if [ "$pin" -gt 28 ] || [ "$pin" = 23 ] || [ "$pin" = 24 ] || [ "$pin" = 25 ]; then
      echo "ERRO: GPIO $pin não pode ser usado (use 0 a 22 ou 26 a 28; o WS é o SCK + 1)." >&2
      exit 2
    fi
  done
  if [ "$AUDIO_PIN_SD" = "$AUDIO_PIN_SCK" ] || [ "$AUDIO_PIN_SD" = "$((AUDIO_PIN_SCK + 1))" ]; then
    echo "ERRO: o SD não pode coincidir com o SCK nem com o WS (SCK + 1)." >&2
    exit 2
  fi
fi

# Esperas do sensor na reassociação: as mesmas faixas que edge_protocol_definitions.h confere na compilação.
check_wait() {
  local option="$1" value="$2" min="$3" max="$4"
  [ -z "$value" ] && return 0
  if ! [[ "$value" =~ ^[0-9]+$ ]] || [ "$value" -lt "$min" ] || [ "$value" -gt "$max" ]; then
    echo "ERRO: $option espera um número de $min a $max ms, e recebeu '$value'." >&2
    exit 2
  fi
}
check_wait --espera-sessao "$WAIT_SESSION" 7000 60000
check_wait --espera-oferta "$WAIT_OFFER" 1000 120000

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

# Sem chave de enlace o sensor anuncia por BLE mas nunca abre sessão. Um aviso no fim de um build longo já
# passou despercebido (CH-056): agora o build não começa, a menos que se peça explicitamente.
if [ -z "$EDGE_LINK_DEVICE_KEY" ] && [ -z "$EDGE_LINK_MASTER_KEY" ] && [ "$ALLOW_NO_KEY" != 1 ]; then
  echo "ERRO: não há chave de enlace para este sensor; o firmware compilado nunca abriria sessão com os módulos." >&2
  echo "  Corrija com ./Codigo/scripts/provision_sensor_security.sh --add-link-key (chave mestra, bancada)" >&2
  echo "  ou ./Codigo/scripts/provision_sensor_security.sh --sensor <UUID> e compile com --sensor <UUID>." >&2
  echo "  Para compilar sem chave de propósito (por exemplo, só para testar a compilação), acrescente --sem-chave." >&2
  exit 3
fi

# Uma pasta por placa e variante: um build não apaga o outro.
BUILD_NAME="$PICO_BOARD_TARGET"
[ -z "$SENSOR_UUID" ] || BUILD_NAME="$BUILD_NAME-$SENSOR_UUID"
[ -z "$AUDIO_PIN_SCK" ] || BUILD_NAME="$BUILD_NAME-mic$AUDIO_PIN_SCK-$AUDIO_PIN_SD"
[ -z "$WAIT_SESSION" ] || BUILD_NAME="$BUILD_NAME-sess$WAIT_SESSION"
[ -z "$WAIT_OFFER" ] || BUILD_NAME="$BUILD_NAME-oferta$WAIT_OFFER"
BUILD_DIR="$FW_DIR/build/$BUILD_NAME"

echo "Compilando $FW_NAME para PICO_BOARD=$PICO_BOARD_TARGET"
echo "Enlace: $LINK_MODE"
if [ -n "$AUDIO_PIN_SCK" ]; then
  echo "Microfone: SCK=GP$AUDIO_PIN_SCK WS=GP$((AUDIO_PIN_SCK + 1)) SD=GP$AUDIO_PIN_SD"
fi

if [ -n "$WAIT_SESSION" ] || [ -n "$WAIT_OFFER" ]; then
  echo "Esperas: sessão=${WAIT_SESSION:-padrão (10000)} ms, oferta de outro módulo=${WAIT_OFFER:-padrão (15000)} ms"
fi

CMAKE_EXTRA=()
if [ -n "$AUDIO_PIN_SCK" ]; then
  CMAKE_EXTRA+=(-DAUDIO_PIN_SCK="$AUDIO_PIN_SCK" -DAUDIO_PIN_SD="$AUDIO_PIN_SD")
fi
[ -z "$WAIT_SESSION" ] || CMAKE_EXTRA+=(-DIOT_SESSION_TIMEOUT_MS="$WAIT_SESSION")
[ -z "$WAIT_OFFER" ] || CMAKE_EXTRA+=(-DIOT_OFFER_SWITCH_HOLD_MS="$WAIT_OFFER")

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake \
  -DPICO_BOARD="$PICO_BOARD_TARGET" \
  -DEDGE_SERIAL_ADMIN_TOKEN="$EDGE_SERIAL_ADMIN_TOKEN" \
  -DEDGE_LINK_DEVICE_KEY="$EDGE_LINK_DEVICE_KEY" \
  -DEDGE_LINK_NETWORK_KEY="$EDGE_LINK_NETWORK_KEY" \
  -DEDGE_LINK_DEVICE_UUID="$EDGE_LINK_DEVICE_UUID" \
  -DEDGE_LINK_MASTER_KEY="$EDGE_LINK_MASTER_KEY" \
  ${CMAKE_EXTRA[@]+"${CMAKE_EXTRA[@]}"} \
  "$FW_DIR"
cmake --build . -j"$(nproc)"

UF2="$BUILD_DIR/$UF2_NAME"
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
  echo "AVISO: compilado com --sem-chave; o plano de dados via Node CAN fica desabilitado neste firmware."
fi
