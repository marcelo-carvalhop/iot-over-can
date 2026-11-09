#!/usr/bin/env bash
# Compila (e, com --upload <porta>, grava) o leitor da documentação no M5Stack PaperColor.
#   build_leitor.sh                        compila
#   build_leitor.sh --upload /dev/ttyACM0  grava depois de compilar
# O texto vem do README e de Documentacao/ no momento da compilação.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PORT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --upload) PORT="${2:-}"; shift 2 ;;
    *) echo "Uso: $0 [--upload <porta>]" >&2; exit 2 ;;
  esac
done
cd "$REPO_ROOT/Codigo/leitor"
if [ -n "$PORT" ]; then
  pio run -t upload --upload-port "$PORT"
else
  pio run
fi
