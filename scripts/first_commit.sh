#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if [ -d ".git" ]; then
  echo "Este diretório já possui .git. Abortando para não sobrescrever histórico."
  exit 1
fi

git init
git branch -M main
git add .
git commit -m "chore: initial iot-over-can baseline"

echo
echo "Primeiro commit criado."
echo
echo "Para publicar no GitHub via SSH:"
echo "git remote add origin git@github.com:<seu-usuario>/iot-over-can.git"
echo "git push -u origin main"
echo
echo "Para publicar via HTTPS:"
echo "git remote add origin https://github.com/<seu-usuario>/iot-over-can.git"
echo "git push -u origin main"
