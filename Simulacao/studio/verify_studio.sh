#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
python3 -m unittest discover -s Simulacao/tests -v
if command -v node >/dev/null 2>&1; then
  node --check Simulacao/studio/static/app.js
fi
./Codigo/scripts/build_network_sim.sh /tmp/ioc_sim_network
bash Simulacao/verify.sh
