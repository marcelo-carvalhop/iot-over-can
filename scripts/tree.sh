#!/usr/bin/env bash
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
find . \
  -path './.git' -prune -o \
  -path './Front/.venv' -prune -o \
  -path './Codigo/node-wifi/build' -prune -o \
          -path './Codigo/node-can/.pio' -prune -o \
  -print | sort
