#!/usr/bin/env bash
set -euo pipefail

NODE_ID="${1:-${IOT_NODE_ID:-4}}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW_DIR="$REPO_ROOT/Codigo/node-can"

cd "$FW_DIR"
IOT_NODE_ID="$NODE_ID" pio run
