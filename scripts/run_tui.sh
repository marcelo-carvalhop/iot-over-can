#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-}"
SECURITY_MODE="${SECURITY_MODE:-presence}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TUI_DIR="$REPO_ROOT/Front"

cd "$TUI_DIR"

if [ ! -d ".venv" ]; then
  echo "Ambiente virtual ainda não existe; executando configuração inicial..."
  "$REPO_ROOT/scripts/setup_tui.sh"
fi

source .venv/bin/activate

if ! command -v iot-over-can-tui >/dev/null 2>&1; then
  echo "Executável iot-over-can-tui não encontrado; atualizando instalação local..."
  python -m pip install -e .
fi

if [ -n "$PORT" ]; then
  exec iot-over-can-tui --port "$PORT" --mode auto --security-mode "$SECURITY_MODE"
else
  exec iot-over-can-tui --mode auto --security-mode "$SECURITY_MODE"
fi
