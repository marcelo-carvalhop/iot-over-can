#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TUI_DIR="$REPO_ROOT/Front"

cd "$TUI_DIR"

if [ ! -d ".venv" ]; then
  python3 -m venv .venv
fi

source .venv/bin/activate
python -m pip install -e '.[dev]'
python -m compileall pico_tui
python -m pytest
