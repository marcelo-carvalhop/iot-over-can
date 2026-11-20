#!/usr/bin/env bash
# Cadastro de operadores da estação e consulta da auditoria (iot-over-can-operators),
# no ambiente virtual da estação e na pasta Front/, onde o run_tui.sh grava o histórico.
#
#   ./Codigo/scripts/operadores.sh add ana --display "Ana Souza"
#   ./Codigo/scripts/operadores.sh list
#   ./Codigo/scripts/operadores.sh audit --operator ana --since 24h
#
# OPERATORS_FILE (opcional) aponta para outro cadastro, o mesmo que o run_tui.sh usa.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT/Front"

if [ ! -d ".venv" ]; then
  echo "Ambiente virtual ainda não existe; executando configuração inicial..."
  "$REPO_ROOT/Codigo/scripts/setup_tui.sh"
fi

source .venv/bin/activate
FILE_ARGS=()
[ -n "${OPERATORS_FILE:-}" ] && FILE_ARGS=(--file "$OPERATORS_FILE")
exec python -m pico_tui.operators_cli ${FILE_ARGS[@]+"${FILE_ARGS[@]}"} "$@"
