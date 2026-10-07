#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOCAL_ENV="$REPO_ROOT/.env.local"
SEC_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/iot-over-can"
SEC_FILE="$SEC_DIR/security.json"
MODE="provision"
SENSOR_UUID=""

usage() {
  cat <<USAGE
Uso: $0 [--check | --renew | --add-link-key | --sensor <UUID64>]

Sem argumento     cria as credenciais apenas quando elas ainda não existem.
--check           verifica presença, formato básico e permissões sem exibir segredos.
--renew           gera novos segredos e substitui .env.local e security.json.
--add-link-key    acrescenta a chave mestra do enlace a um .env.local existente,
                  preservando o token de manutenção já gravado nos sensores.
--sensor <UUID>   deriva as chaves de um sensor (K_dev e K_net) e grava
                  .env.sensor-<UUID>.local para o build do Pico W. O UUID aparece
                  em VERSION no console do sensor e na tela Sensores sem fio.

Arquivos gerados (permissão 600, ignorados pelo Git):
  .env.local                   token de manutenção + K_master (Nodes CAN)
  .env.sensor-<UUID>.local     token de manutenção + K_dev + K_net (um sensor)
  ~/.config/iot-over-can/security.json   configuração da TUI
USAGE
}

check_permissions() {
  local path="$1"
  local mode
  mode="$(stat -c '%a' "$path")"
  if [ "$mode" != "600" ]; then
    echo "ERRO: $path deve usar permissão 600; atual=$mode" >&2
    return 1
  fi
}

check_files() {
  local failed=0
  if [ ! -f "$LOCAL_ENV" ]; then
    echo "AUSENTE: $LOCAL_ENV"
    failed=1
  else
    check_permissions "$LOCAL_ENV" || failed=1
    grep -q '^EDGE_SERIAL_ADMIN_TOKEN=0x[0-9A-Fa-f]\{16\}$' "$LOCAL_ENV" || {
      echo "ERRO: token serial ausente ou inválido em $LOCAL_ENV" >&2
      failed=1
    }
    grep -q '^EDGE_LINK_MASTER_KEY=[0-9A-Fa-f]\{64\}$' "$LOCAL_ENV" || {
      echo "ERRO: chave mestra do enlace ausente ou inválida em $LOCAL_ENV" >&2
      echo "      Use '$0 --add-link-key' para acrescentá-la sem trocar o token serial." >&2
      failed=1
    }
    if grep -q '^EDGE_NODE_PRESHARED_KEY=' "$LOCAL_ENV"; then
      echo "AVISO: EDGE_NODE_PRESHARED_KEY (protocolo v5) não é mais utilizada e pode ser removida."
    fi
  fi

  if [ ! -f "$SEC_FILE" ]; then
    echo "AUSENTE: $SEC_FILE"
    failed=1
  else
    check_permissions "$SEC_FILE" || failed=1
    python3 - "$SEC_FILE" <<'PY' || failed=1
import json, pathlib, re, sys
p = pathlib.Path(sys.argv[1])
try:
    data = json.loads(p.read_text())
except Exception as exc:
    raise SystemExit(f"ERRO: JSON inválido em {p}: {exc}")
token = str(data.get("device_admin_token", ""))
if not re.fullmatch(r"0x[0-9A-Fa-f]{16}", token):
    raise SystemExit(f"ERRO: device_admin_token ausente ou inválido em {p}")
if int(data.get("unlock_seconds", 0)) <= 0:
    raise SystemExit(f"ERRO: unlock_seconds inválido em {p}")
print(f"OK: {p}")
PY
  fi

  local sensor_env
  for sensor_env in "$REPO_ROOT"/.env.sensor-*.local; do
    [ -e "$sensor_env" ] || continue
    check_permissions "$sensor_env" || failed=1
    echo "OK: $sensor_env"
  done

  if [ "$failed" -ne 0 ]; then
    return 1
  fi
  echo "OK: $LOCAL_ENV"
  echo "Credenciais locais válidas e protegidas."
}

read_env_value() {
  # Lê NOME=valor de .env.local sem executar o arquivo.
  local name="$1"
  sed -n "s/^${name}=//p" "$LOCAL_ENV" | head -n 1
}

new_link_key() {
  python3 -c 'import secrets; print(secrets.token_hex(32))'
}

case "${1:-}" in
  "") MODE="provision" ;;
  --check) MODE="check" ;;
  --renew) MODE="renew" ;;
  --add-link-key) MODE="add-link-key" ;;
  --sensor)
    MODE="sensor"
    SENSOR_UUID="${2:-}"
    if [ -z "$SENSOR_UUID" ]; then usage >&2; exit 2; fi
    ;;
  -h|--help) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

if [ "$MODE" = "check" ]; then
  check_files
  exit $?
fi

if [ "$MODE" = "add-link-key" ]; then
  if [ ! -f "$LOCAL_ENV" ]; then
    echo "ERRO: $LOCAL_ENV não existe; execute '$0' sem argumentos." >&2
    exit 3
  fi
  if grep -q '^EDGE_LINK_MASTER_KEY=' "$LOCAL_ENV"; then
    echo "A chave mestra do enlace já existe. Nenhum arquivo foi alterado." >&2
    exit 3
  fi
  umask 077
  printf 'EDGE_LINK_MASTER_KEY=%s\n' "$(new_link_key)" >> "$LOCAL_ENV"
  chmod 600 "$LOCAL_ENV"
  echo "Chave mestra do enlace acrescentada a $LOCAL_ENV."
  echo "Recompile e grave os Nodes CAN; depois gere as chaves de cada sensor com '$0 --sensor <UUID>'."
  exit 0
fi

if [ "$MODE" = "sensor" ]; then
  if [ ! -f "$LOCAL_ENV" ]; then
    echo "ERRO: $LOCAL_ENV não existe; execute '$0' sem argumentos." >&2
    exit 3
  fi
  MASTER_KEY="$(read_env_value EDGE_LINK_MASTER_KEY)"
  SERIAL_TOKEN="$(read_env_value EDGE_SERIAL_ADMIN_TOKEN)"
  if ! [[ "$MASTER_KEY" =~ ^[0-9A-Fa-f]{64}$ ]]; then
    echo "ERRO: chave mestra do enlace ausente em $LOCAL_ENV; use '$0 --add-link-key'." >&2
    exit 3
  fi
  # A chave mestra chega ao Python pelo ambiente, não pela linha de comando,
  # para não aparecer na lista de processos.
  DERIVED="$(IOC_MASTER="$MASTER_KEY" python3 - "$SENSOR_UUID" <<'PY'
import hashlib, hmac, os, sys
text = sys.argv[1].strip()
try:
    uuid = int(text, 16)
except ValueError:
    raise SystemExit(f"ERRO: UUID inválido: {text!r}. Use 16 dígitos hexadecimais.")
if not 0 < uuid < 1 << 64:
    raise SystemExit("ERRO: UUID fora da faixa de 64 bits.")
master = bytes.fromhex(os.environ["IOC_MASTER"])
mac = lambda key, msg: hmac.new(key, msg, hashlib.sha256).digest()
device = mac(master, b"ioc-dev-v1" + uuid.to_bytes(8, "big"))
network = mac(master, b"ioc-net-v1")
print(f"{uuid:016X} {device.hex()} {network.hex()}")
PY
)"
  read -r UUID_HEX DEVICE_KEY NETWORK_KEY <<<"$DERIVED"
  SENSOR_ENV="$REPO_ROOT/.env.sensor-$UUID_HEX.local"
  umask 077
  cat > "$SENSOR_ENV" <<EOF_SENSOR
EDGE_SERIAL_ADMIN_TOKEN=${SERIAL_TOKEN:-0}
EDGE_LINK_DEVICE_UUID=$UUID_HEX
EDGE_LINK_DEVICE_KEY=$DEVICE_KEY
EDGE_LINK_NETWORK_KEY=$NETWORK_KEY
EOF_SENSOR
  chmod 600 "$SENSOR_ENV"
  echo "Chaves do sensor 0x$UUID_HEX gravadas em $SENSOR_ENV."
  echo "Compile o firmware deste sensor com:"
  echo "  ./Codigo/scripts/build_pico.sh --sensor $UUID_HEX"
  echo "Nenhum segredo foi exibido no terminal."
  exit 0
fi

if [ "$MODE" = "provision" ] && { [ -e "$LOCAL_ENV" ] || [ -e "$SEC_FILE" ]; }; then
  echo "Credenciais já existem. Nenhum arquivo foi alterado." >&2
  echo "Use '$0 --check' para validar ou '$0 --renew' para rotacionar." >&2
  exit 3
fi

SERIAL_TOKEN="$(python3 -c 'import secrets; print(f"0x{(secrets.randbits(64) or 1):016X}")')"
LINK_KEY="$(new_link_key)"

umask 077
cat > "$LOCAL_ENV" <<EOF_ENV
EDGE_SERIAL_ADMIN_TOKEN=$SERIAL_TOKEN
EDGE_LINK_MASTER_KEY=$LINK_KEY
EOF_ENV
chmod 600 "$LOCAL_ENV"

mkdir -p "$SEC_DIR"
cat > "$SEC_FILE" <<EOF_JSON
{
  "device_admin_token": "$SERIAL_TOKEN",
  "unlock_seconds": 300,
  "otp_public_ids": []
}
EOF_JSON
chmod 600 "$SEC_FILE"

if [ "$MODE" = "renew" ]; then
  # Chaves derivadas da chave mestra anterior deixam de valer.
  rm -f "$REPO_ROOT"/.env.sensor-*.local
  echo "Credenciais rotacionadas. Recompile e grave novamente os Nodes CAN e os sensores,"
  echo "gerando antes as chaves de cada sensor com '$0 --sensor <UUID>'."
else
  echo "Credenciais locais provisionadas."
fi
printf '  build env: %s\n' "$LOCAL_ENV"
printf '  TUI config: %s\n' "$SEC_FILE"
printf 'Nenhum segredo foi exibido no terminal.\n'
printf 'Use "%s --check" para verificar a instalação.\n' "$0"
