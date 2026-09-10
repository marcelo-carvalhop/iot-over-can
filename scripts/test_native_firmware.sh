#!/usr/bin/env bash
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW="$REPO_ROOT/Codigo/node-wifi"
OUT="${TMPDIR:-/tmp}/iot-over-can-config-validation-test"
cc -std=c11 -Wall -Wextra -Werror \
  -I"$FW" \
  "$FW/config_validation.c" \
  "$FW/tests/test_config_validation.c" \
  -lm -o "$OUT"
"$OUT"
rm -f "$OUT"
echo "native firmware validation tests: PASS"
