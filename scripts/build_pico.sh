#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW_DIR="$REPO_ROOT/firmware/pico-edge-sensor"

cd "$FW_DIR"
rm -rf build
mkdir -p build
cd build
cmake -DPICO_BOARD=pico2_w ..
cmake --build . -j"$(nproc)"
echo
echo "Build finalizado. Arquivos gerados em: $FW_DIR/build"
