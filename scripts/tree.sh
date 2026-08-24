#!/usr/bin/env bash
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
find . \
  -path './.git' -prune -o \
  -path './software/tui/.venv' -prune -o \
  -path './firmware/pico-edge-sensor/build' -prune -o \
  -print | sort
