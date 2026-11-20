#!/usr/bin/env bash
# Compila e grava o firmware de um Módulo CAN (ID 1 a 31) ou da Probe 00 (ID 0).
#
#   ./Codigo/scripts/upload_esp32_can_node.sh <NODE_ID 0..31> [UPLOAD_PORT] [--sem-chave] [--auth-can]
#
# Os módulos precisam da chave mestra do enlace (.env.local); sem ela o script
# não grava, a menos que --sem-chave seja pedido (ver build_esp32_can_node.sh).
# --auth-can liga a autenticação de origem dos comandos (grave todos os módulos e a Probe com ela).
set -euo pipefail

NODE_ID=""
UPLOAD_PORT=""
ALLOW_NO_KEY=0
AUTH_CAN=0
for arg in "$@"; do
  case "$arg" in
    --sem-chave) ALLOW_NO_KEY=1 ;;
    --auth-can) AUTH_CAN=1 ;;
    -h|--help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "Opção desconhecida: $arg" >&2; exit 2 ;;
    *)
      if [ -z "$NODE_ID" ]; then NODE_ID="$arg"; else UPLOAD_PORT="$arg"; fi
      ;;
  esac
done

if [ -z "$NODE_ID" ]; then
  echo "Uso: $0 <NODE_ID 0..31> [UPLOAD_PORT] [--sem-chave] [--auth-can]"
  echo "Exemplos:"
  echo "  $0 1 /dev/ttyUSB0"
  echo "  $0 2"
  exit 1
fi
if ! [[ "$NODE_ID" =~ ^[0-9]+$ ]] || [ "$NODE_ID" -lt 0 ] || [ "$NODE_ID" -gt 31 ]; then
  echo "ERRO: NODE_ID deve estar entre 0 e 31." >&2
  exit 2
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FW_DIR="$REPO_ROOT/Codigo/node-can"

# A chave mestra do enlace wireless vem de .env.local e só é necessária nos
# Nodes funcionais; a Probe 00 não participa do plano de dados com sensores
# (com --auth-can ela também precisa da chave, para etiquetar os comandos).
LOCAL_ENV="$REPO_ROOT/.env.local"
IOT_LINK_MASTER_KEY=""
NEEDS_KEY=0
if [ "$NODE_ID" -ne 0 ] || [ "$AUTH_CAN" = 1 ]; then NEEDS_KEY=1; fi
if [ "$NEEDS_KEY" = 1 ] && [ -f "$LOCAL_ENV" ]; then
  IOT_LINK_MASTER_KEY="$(sed -n 's/^EDGE_LINK_MASTER_KEY=//p' "$LOCAL_ENV" | head -n 1)"
fi
export IOT_LINK_MASTER_KEY
# O script decide: um IOT_CAN_AUTH herdado do ambiente não liga a opção sem que ela tenha sido pedida.
export IOT_CAN_AUTH="$AUTH_CAN"
if [ "$AUTH_CAN" = 1 ] && [ "$ALLOW_NO_KEY" = 1 ]; then
  echo "ERRO: --auth-can exige a chave de enlace e não combina com --sem-chave." >&2
  exit 2
fi
if [ "$AUTH_CAN" = 1 ] && [ -z "$IOT_LINK_MASTER_KEY" ]; then
  echo "ERRO: --auth-can exige a chave de enlace em .env.local (a Probe a usa para etiquetar os comandos)." >&2
  echo "  Corrija com ./Codigo/scripts/provision_sensor_security.sh --add-link-key e grave de novo." >&2
  exit 3
fi
if [ "$NODE_ID" -ne 0 ] && [ -z "$IOT_LINK_MASTER_KEY" ]; then
  if [ "$ALLOW_NO_KEY" != 1 ]; then
    echo "ERRO: chave de enlace ausente em .env.local; o módulo $NODE_ID nunca abriria o ponto de acesso dos sensores." >&2
    echo "  Corrija com ./Codigo/scripts/provision_sensor_security.sh --add-link-key e grave de novo." >&2
    echo "  Para gravar sem chave de propósito, acrescente --sem-chave." >&2
    exit 3
  fi
  echo "AVISO: --sem-chave: o plano de dados wireless ficará desabilitado neste módulo." >&2
  export IOT_ALLOW_NO_LINK_KEY=1
fi

cd "$FW_DIR"

if [ -n "$UPLOAD_PORT" ]; then
  IOT_NODE_ID="$NODE_ID" pio run -t upload --upload-port "$UPLOAD_PORT"
else
  IOT_NODE_ID="$NODE_ID" pio run -t upload
fi
