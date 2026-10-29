#!/usr/bin/env bash
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FW="$REPO_ROOT/Codigo/node-wifi"
LINK="$REPO_ROOT/Codigo/common/ioc_link"
NODE="$REPO_ROOT/Codigo/node-can"
OUT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/iot-over-can-native.XXXXXX")"
trap 'rm -rf "$OUT_DIR"' EXIT

# 1. Validação de configuração do sensor.
cc -std=c11 -Wall -Wextra -Werror \
  -I"$FW" \
  "$FW/config_validation.c" \
  "$FW/tests/test_config_validation.c" \
  -lm -o "$OUT_DIR/config_validation"
"$OUT_DIR/config_validation"

# 2. Biblioteca compartilhada: SHA-256/HMAC, autenticação do enlace e
#    segmentação do plano de dados no CAN.
cc -std=c11 -Wall -Wextra -Werror \
  -I"$LINK" \
  "$LINK/ioc_sha256.c" "$LINK/ioc_link.c" "$LINK/ioc_wdata.c" \
  "$REPO_ROOT/Codigo/common/tests/test_ioc_link.c" \
  -lm -o "$OUT_DIR/test_ioc_link"
"$OUT_DIR/test_ioc_link"

# 2b. Custo da criptografia do enlace (ioc_bench): estatísticas e formato das
#     linhas BENCH; a mesma rotina roda no Node, no Pico W e aqui como referência.
cc -std=c11 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
  -I"$LINK" \
  "$LINK/ioc_sha256.c" "$LINK/ioc_link.c" "$LINK/ioc_bench.c" \
  "$REPO_ROOT/Codigo/common/tests/test_ioc_bench.c" \
  -o "$OUT_DIR/test_ioc_bench"
"$OUT_DIR/test_ioc_bench"

# 3. Medição de ocupação do barramento, PDU autenticado (predisposição para
#    CAN FD), segmentação em quadros maiores, reassociação automática e a
#    tabela de vínculos gravada pelos Nodes.
cc -std=c11 -Wall -Wextra -Werror \
  -I"$LINK" \
  "$LINK/ioc_sha256.c" "$LINK/ioc_wdata.c" "$LINK/ioc_canbits.c" \
  "$LINK/ioc_secpdu.c" "$LINK/ioc_failover.c" "$LINK/ioc_bindstore.c" \
  "$REPO_ROOT/Codigo/common/tests/test_ioc_net.c" \
  -lm -o "$OUT_DIR/test_ioc_net"
"$OUT_DIR/test_ioc_net"

# 3-painel. Painel em papel eletrônico: formatos, retrato do Node, visão da
#     rede, composição da página e política de atualização (ASan/UBSan quando há).
PANEL="$REPO_ROOT/Codigo/common/ioc_panel"
PANEL_FLAGS=(-std=c11 -O1 -g -Wall -Wextra -Werror)
if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined - -o "$OUT_DIR/san_probe" 2>/dev/null &&
   "$OUT_DIR/san_probe" 2>/dev/null; then
  PANEL_FLAGS+=(-fsanitize=address,undefined -fno-sanitize-recover=all)
fi
cc "${PANEL_FLAGS[@]}" -I"$LINK" -I"$PANEL" \
  "$LINK/ioc_sha256.c" "$LINK/ioc_wdata.c" "$LINK/ioc_canbits.c" \
  "$PANEL/ioc_panel_proto.c" "$PANEL/ioc_panel_cache.c" "$PANEL/ioc_view.c" "$PANEL/ioc_page.c" \
  "$REPO_ROOT/Codigo/common/tests/test_ioc_panel.c" \
  -lm -o "$OUT_DIR/test_ioc_panel"
"$OUT_DIR/test_ioc_panel"

# 3a. Fuzz determinístico dos decodificadores do enlace e do plano de dados
#     (oferta BLE, envelope UDP, handshake, relatórios, segmentos, PDU
#     autenticada): entradas aleatórias e mensagens válidas mutadas, em buffers
#     do tamanho exato, com AddressSanitizer/UBSan quando o compilador oferece.
FUZZ_FLAGS=(-std=c11 -O1 -g -Wall -Wextra -Werror)
if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined - -o "$OUT_DIR/san_probe" 2>/dev/null &&
   "$OUT_DIR/san_probe" 2>/dev/null; then
  FUZZ_FLAGS+=(-fsanitize=address,undefined -fno-sanitize-recover=all)
fi
cc "${FUZZ_FLAGS[@]}" -I"$LINK" \
  "$LINK/ioc_sha256.c" "$LINK/ioc_link.c" "$LINK/ioc_wdata.c" "$LINK/ioc_canbits.c" "$LINK/ioc_secpdu.c" "$LINK/ioc_bindstore.c" \
  "$REPO_ROOT/Codigo/common/tests/fuzz_ioc_decoders.c" \
  -lm -o "$OUT_DIR/fuzz_ioc_decoders"
"$OUT_DIR/fuzz_ioc_decoders" 50000 > /dev/null

# 3b. Sensor acústico (Codigo/node-audio): motor de DSP (níveis ponderados,
#     doses, Ln, eventos, tacômetro acústico, anomalia) e validação da
#     configuração. O DSP roda também com AddressSanitizer/UBSan quando o
#     compilador oferece os dois.
AUDIO="$REPO_ROOT/Codigo/node-audio"
AUDIO_SOURCES=("$AUDIO/acoustic_dsp.c" "$AUDIO/audio_fft.c" "$AUDIO/tests/test_acoustic_dsp.c")
cc -std=c11 -O2 -Wall -Wextra -Werror -I"$AUDIO" -I"$FW" \
  "${AUDIO_SOURCES[@]}" -lm -o "$OUT_DIR/test_acoustic_dsp"
"$OUT_DIR/test_acoustic_dsp"
if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined - -o "$OUT_DIR/san_probe" 2>/dev/null &&
   "$OUT_DIR/san_probe" 2>/dev/null; then
  cc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Werror \
    -I"$AUDIO" -I"$FW" "${AUDIO_SOURCES[@]}" -lm -o "$OUT_DIR/test_acoustic_dsp_san"
  "$OUT_DIR/test_acoustic_dsp_san" > /dev/null
fi
cc -std=c11 -Wall -Wextra -Werror -I"$AUDIO" -I"$FW" \
  "$AUDIO/audio_config.c" "$AUDIO/acoustic_dsp.c" "$AUDIO/audio_fft.c" "$AUDIO/tests/test_audio_config.c" \
  -lm -o "$OUT_DIR/test_audio_config"
"$OUT_DIR/test_audio_config"

# 3b. Amostragem de Poisson das sondas de eco (instrumentação de pesquisa).
c++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -I"$REPO_ROOT/Codigo/node-can/test/host/stubs" -I"$REPO_ROOT/Codigo/node-can/include" \
  "$REPO_ROOT/Codigo/node-can/test/host/test_pesquisa.cpp" -lm -o "$OUT_DIR/test_pesquisa"
"$OUT_DIR/test_pesquisa"

# 4. Simulação sensor <-> Node <-> Probe com o código real de wireless_link.cpp.
"$REPO_ROOT/Codigo/scripts/build_wireless_link_sim.sh" "$OUT_DIR/sim_wireless_link"
"$OUT_DIR/sim_wireless_link" > /dev/null

# 5. Bancada virtual: Probe 00, três Nodes (firmware real) e sensores
#    simulados. Cada cenário é um processo, pois um reinício de Node consome
#    uma instância do firmware.
"$REPO_ROOT/Codigo/scripts/build_network_sim.sh" "$OUT_DIR/sim_network"
while IFS=$'\t' read -r scenario _description; do
  "$OUT_DIR/sim_network" "$scenario" > /dev/null
done < <("$OUT_DIR/sim_network" --list)

echo "native firmware validation tests: PASS"
