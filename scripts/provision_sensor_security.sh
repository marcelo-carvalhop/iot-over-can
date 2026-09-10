#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCAL_ENV="$REPO_ROOT/.env.local"
SEC_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/iot-over-can"
SEC_FILE="$SEC_DIR/security.json"

read -r SERIAL_TOKEN UDP_KEY < <(python3 - <<'PY'
import secrets
serial = secrets.randbits(64) or 1
udp = secrets.randbits(64) or 1
print(f"0x{serial:016X} 0x{udp:016X}")
PY
)

umask 077
cat > "$LOCAL_ENV" <<EOF
EDGE_SERIAL_ADMIN_TOKEN=$SERIAL_TOKEN
EDGE_NODE_PRESHARED_KEY=$UDP_KEY
EDGE_ALLOW_LEGACY_INSECURE_UDP_CONTROL=0
EOF
chmod 600 "$LOCAL_ENV"

mkdir -p "$SEC_DIR"
cat > "$SEC_FILE" <<EOF
{
  "device_admin_token": "$SERIAL_TOKEN",
  "unlock_seconds": 300,
  "otp_public_ids": []
}
EOF
chmod 600 "$SEC_FILE"

printf 'Credenciais locais provisionadas.\n'
printf '  build env: %s\n' "$LOCAL_ENV"
printf '  TUI config: %s\n' "$SEC_FILE"
printf 'A chave UDP legado foi gerada separadamente e permanece apenas em .env.local.\n'
printf 'Não adicione .env.local ao Git.\n'
