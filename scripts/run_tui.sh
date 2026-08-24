#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/ttyACM0}"
SECURITY_MODE="${SECURITY_MODE:-presence}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TUI_DIR="$REPO_ROOT/software/tui"

cd "$TUI_DIR"

if [ ! -d ".venv" ]; then
  python3 -m venv .venv
fi

source .venv/bin/activate
python -m pip install -e .
pico-tui --port "$PORT" --mode sensor --security-mode "$SECURITY_MODE"
