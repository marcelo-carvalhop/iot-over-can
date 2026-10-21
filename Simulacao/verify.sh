#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo '[1/5] Validando geracao de fabricas e Python'
python3 -m unittest discover -s Simulacao/tests -v

echo '[2/5] Compilando firmware real com Nodes 1 a 6 (2 geracoes)'
IOC_SIM_MAX_NODES=6 IOC_SIM_GENERATIONS=2 ./Codigo/scripts/build_network_sim.sh "$TMP/sim_network"

echo '[3/5] Conferindo compatibilidade com os 17 cenarios historicos'
"$TMP/sim_network" --list > "$TMP/list.txt"
[[ "$(wc -l < "$TMP/list.txt")" == 17 ]]

echo '[4/5] Criacao e remocao em runtime, e sensores sem fio'
printf 'PAUSE\nNODE 4 ADD\nNODE 4 BUDGET 60\nSENSOR ADD 0xE6616408432B6F39 VIBRATION\nSENSOR RSSI 0xE6616408432B6F39 4 -53\nSENSOR LINK 0xE6616408432B6F39 4 LOSS 25\nSTEP 2000\nSTATUS\nNODE 4 REMOVE\nSTATUS\nNODE 4 ADD\nSTATUS\nQUIT\n' | "$TMP/sim_network" --interactive > "$TMP/session.log"
grep -q '#CTRL OK NODE id=4 kind=ADD' "$TMP/session.log"
grep -q '#CTRL OK NODE id=4 kind=REMOVE' "$TMP/session.log"
grep -q 'node=4 rssi=-53' "$TMP/session.log"
grep -q 'loss_pct=25' "$TMP/session.log"
grep -q 'kind=BUDGET pct=60' "$TMP/session.log"
grep -q '#CTRL OK STEP' "$TMP/session.log"
! grep -q '#CTRL ERROR' "$TMP/session.log"

echo '[5/5] Exercitando cenário de regressao nativo'
"$TMP/sim_network" formation > "$TMP/formation.log"
echo 'PASS: testes Python, compilacao, operacao interativa e cenario formation'
echo 'Opcional: ./Codigo/scripts/test_native_firmware.sh (suite completa)'
