#!/usr/bin/env bash
# Abre a estação de operação (TUI) no ambiente virtual de Front/, que é onde o
# histórico (logs/) e as exportações (exports/) são gravados.
#
#   ./Codigo/scripts/run_tui.sh [PORTA] [opções da estação...]
#
# Variáveis de ambiente (todas opcionais):
#   SECURITY_MODE    presence (padrão), otp ou off: autorização por chave física
#   OPERATOR_MODE    auto (padrão), required ou off: identificação do operador
#   OPERATOR_IDLE    minutos sem atividade até a sessão do operador expirar (padrão da estação: 15; 0 desliga)
#   OPERATORS_FILE   cadastro de operadores (padrão: ~/.config/iot-over-can/operators.json)
#
# O que vier depois da porta vai direto para a estação, para qualquer opção que
# não tenha variável, por exemplo:
#   ./Codigo/scripts/run_tui.sh /dev/ttyUSB0 --operator-idle 1 --ascii
set -euo pipefail

PORT="${1:-}"
[ $# -gt 0 ] && shift
SECURITY_MODE="${SECURITY_MODE:-presence}"
OPERATOR_MODE="${OPERATOR_MODE:-auto}"
OPERATOR_IDLE="${OPERATOR_IDLE:-}"
OPERATORS_FILE="${OPERATORS_FILE:-}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TUI_DIR="$REPO_ROOT/Front"

cd "$TUI_DIR"

if [ ! -d ".venv" ]; then
  echo "Ambiente virtual ainda não existe; executando configuração inicial..."
  "$REPO_ROOT/Codigo/scripts/setup_tui.sh"
fi

source .venv/bin/activate

if ! command -v iot-over-can-tui >/dev/null 2>&1; then
  echo "Executável iot-over-can-tui não encontrado; atualizando instalação local..."
  python -m pip install -e .
fi

ARGS=(--mode auto --security-mode "$SECURITY_MODE" --operator-mode "$OPERATOR_MODE")
[ -n "$PORT" ] && ARGS+=(--port "$PORT")
[ -n "$OPERATOR_IDLE" ] && ARGS+=(--operator-idle "$OPERATOR_IDLE")
[ -n "$OPERATORS_FILE" ] && ARGS+=(--operators-file "$OPERATORS_FILE")

exec iot-over-can-tui "${ARGS[@]}" "$@"
