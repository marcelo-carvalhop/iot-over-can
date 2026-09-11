#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOCAL_ENV="$REPO_ROOT/.env.local"
SEC_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/iot-over-can"
SEC_FILE="$SEC_DIR/security.json"
MODE="provision"

usage() {
  cat <<USAGE
Uso: $0 [--check|--renew]

Sem argumento  cria as credenciais apenas quando elas ainda não existem.
--check        verifica presença, formato básico e permissões sem exibir segredos.
--renew        gera novos segredos e substitui .env.local e security.json.
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
    grep -q '^EDGE_NODE_PRESHARED_KEY=0x[0-9A-Fa-f]\{16\}$' "$LOCAL_ENV" || {
      echo "ERRO: chave de controle UDP ausente ou inválida em $LOCAL_ENV" >&2
      failed=1
    }
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

  if [ "$failed" -ne 0 ]; then
    return 1
  fi
  echo "OK: $LOCAL_ENV"
  echo "Credenciais locais válidas e protegidas."
}

case "${1:-}" in
  "") MODE="provision" ;;
  --check) MODE="check" ;;
  --renew) MODE="renew" ;;
  -h|--help) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

if [ "$MODE" = "check" ]; then
  check_files
  exit $?
fi

if [ "$MODE" = "provision" ] && { [ -e "$LOCAL_ENV" ] || [ -e "$SEC_FILE" ]; }; then
  echo "Credenciais já existem. Nenhum arquivo foi alterado." >&2
  echo "Use '$0 --check' para validar ou '$0 --renew' para rotacionar." >&2
  exit 3
fi

read -r SERIAL_TOKEN UDP_KEY < <(python3 - <<'PY'
import secrets
serial = secrets.randbits(64) or 1
udp = secrets.randbits(64) or 1
print(f"0x{serial:016X} 0x{udp:016X}")
PY
)

umask 077
cat > "$LOCAL_ENV" <<EOF_ENV
EDGE_SERIAL_ADMIN_TOKEN=$SERIAL_TOKEN
EDGE_NODE_PRESHARED_KEY=$UDP_KEY
EDGE_ALLOW_LEGACY_INSECURE_UDP_CONTROL=0
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
  echo "Credenciais rotacionadas. Recompile e grave novamente o sensor wireless."
else
  echo "Credenciais locais provisionadas."
fi
printf '  build env: %s\n' "$LOCAL_ENV"
printf '  TUI config: %s\n' "$SEC_FILE"
printf 'Nenhum segredo foi exibido no terminal.\n'
printf 'Use "%s --check" para verificar a instalação.\n' "$0"
