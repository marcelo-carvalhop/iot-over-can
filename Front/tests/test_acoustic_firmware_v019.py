"""Firmware do sensor acústico INMP441 (v0.19).

1. O programa da PIO (Codigo/node-audio/inmp441_i2s.pio) é interpretado por
   um simulador mínimo e ligado a um modelo do transmissor I2S do INMP441:
   as palavras entregues ao FIFO precisam sair alinhadas, sem troca de canal,
   com o canal esquerdo e com o direito (WS invertido no pino).
2. Contrato de código-fonte: o firmware compila para Pico W e Pico 2 W,
   reaproveita o enlace autenticado do sensor de vibração e mantém o DSP no
   núcleo 1.
3. Os testes nativos do motor DSP (acoustic_dsp.c) rodam quando há compilador.
"""
from __future__ import annotations

import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
AUDIO = ROOT / "Codigo" / "node-audio"
WIFI = ROOT / "Codigo" / "node-wifi"


# ---------------------------------------------------------------------------
# 1. Simulação do programa da PIO
# ---------------------------------------------------------------------------


@dataclass
class Instruction:
    op: str
    args: tuple[str, ...]
    side: int


def parse_pio(path: Path) -> tuple[list[Instruction], int, int, dict[str, int]]:
    """Instruções, índices de .wrap_target e .wrap e rótulos."""

    text = path.read_text(encoding="utf-8")
    body = text[text.index(".program") : text.index("% c-sdk")]
    instructions: list[Instruction] = []
    labels: dict[str, int] = {}
    wrap_target = 0
    wrap = -1
    for raw in body.splitlines()[1:]:
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        if line.startswith(".side_set"):
            assert line.split()[1] == "2"
            continue
        if line == ".wrap_target":
            wrap_target = len(instructions)
            continue
        if line == ".wrap":
            wrap = len(instructions) - 1
            continue
        if line.endswith(":"):
            labels[line[:-1].removeprefix("public ").strip()] = len(instructions)
            continue
        match = re.fullmatch(r"(\w+)\s*(.*?)\s+side\s+0b([01]{2})", line)
        assert match, f"instrução não reconhecida: {line}"
        op, operands, side = match.group(1), match.group(2), int(match.group(3), 2)
        args = tuple(part for part in re.split(r"[,\s]+", operands.strip()) if part)
        instructions.append(Instruction(op, args, side))
    resolved = [
        Instruction(i.op, tuple(str(labels[a]) if a in labels else a for a in i.args), i.side) for i in instructions
    ]
    return resolved, wrap_target, wrap, labels


def simulate(words: list[int], *, right_channel: bool = False) -> tuple[list[int], int]:
    """Executa o programa com um INMP441 simulado; devolve as palavras do FIFO
    e o número de instruções por quadro."""

    program, wrap_target, wrap, labels = parse_pio(AUDIO / "inmp441_i2s.pio")
    # Como no firmware: partida em `entry` com SCK e WS em nível alto.
    pc, x = labels["entry"], 0
    sck = 1
    isr, isr_bits = 0, 0
    pushed: list[int] = []

    # Transmissor I2S do INMP441: troca o bit na descida de SCK. Na descida em
    # que WS passa para o seu slot ainda sai o LSB do outro canal (linha livre,
    # 0 pelo pull-down); o MSB sai na descida seguinte, e o seu LSB, na descida
    # em que WS deixa o seu slot. Com L/R em VDD (canal direito) o slot é WS = 1.
    own_ws = 1 if right_channel else 0
    pending = list(words)
    previous_ws_pin = 1 ^ (1 if right_channel else 0)   # WS interno começa em 1
    current: int | None = None
    bit_index: int | None = None
    sd = 0

    ws_rises: list[int] = []
    previous_ws_internal = 1
    total_cycles = (len(words) + 2) * 128
    for cycle in range(total_cycles):
        instruction = program[pc]
        new_sck = instruction.side & 1
        ws_internal = (instruction.side >> 1) & 1
        if ws_internal == 1 and previous_ws_internal == 0:
            ws_rises.append(cycle)
        previous_ws_internal = ws_internal
        falling = sck == 1 and new_sck == 0
        sck = new_sck
        ws_pin = ws_internal ^ (1 if right_channel else 0)   # gpio_set_outover(INVERT)
        if falling:
            if ws_pin != previous_ws_pin:
                if ws_pin == own_ws:
                    sd = 0
                    current = pending.pop(0) if pending else 0
                    bit_index = 31
                else:
                    sd = (current & 1) if current is not None else 0
                    current, bit_index = None, None
                previous_ws_pin = ws_pin
            elif ws_pin == own_ws and current is not None and bit_index is not None and bit_index >= 1:
                sd = (current >> bit_index) & 1
                bit_index -= 1
            else:
                sd = 0

        next_pc = pc + 1
        if instruction.op == "in":
            assert instruction.args == ("pins", "1")
            isr = ((isr << 1) | sd) & 0xFFFFFFFF
            isr_bits += 1
            if isr_bits == 32:                 # autopush de 32 bits
                pushed.append(isr)
                isr, isr_bits = 0, 0
        elif instruction.op == "jmp":
            condition, target = instruction.args
            assert condition == "x--"
            if x != 0:
                next_pc = int(target)
            x = (x - 1) & 0xFFFFFFFF
        elif instruction.op == "set":
            register, value = instruction.args
            assert register == "x"
            x = int(value)
        elif instruction.op != "nop":
            raise AssertionError(f"instrução inesperada: {instruction.op}")
        pc = wrap_target if pc == wrap else next_pc
    per_frame = ws_rises[-1] - ws_rises[-2] if len(ws_rises) >= 2 else 0
    return pushed, per_frame


def _words() -> list[int]:
    samples = [0x123456, -0x000001, 0x7FFFFF, -0x800000, 0x0F0F0F, 0x00A5A5, -0x123456, 0x400000]
    return [((s & 0xFFFFFF) << 8) for s in samples]


@pytest.mark.parametrize("right", [False, True], ids=["canal_esquerdo", "canal_direito"])
def test_pio_program_delivers_aligned_24_bit_words(right: bool) -> None:
    words = _words()
    pushed, per_frame = simulate(words, right_channel=right)
    # 64 períodos de SCK por quadro, duas instruções por período.
    assert per_frame == 128
    # Cada palavra sai inteira, na ordem, uma por quadro, sem bits do outro canal.
    assert pushed[: len(words)] == words
    for word, expected in zip(pushed, words):
        assert word & 0xFF == 0                       # 8 bits de preenchimento zerados
        sample = (word >> 8) - (1 << 24) if word & 0x80000000 else word >> 8
        assert sample == ((expected >> 8) - (1 << 24) if expected & 0x80000000 else expected >> 8)


def test_pio_program_fits_and_samples_only_on_rising_edges() -> None:
    program, wrap_target, wrap, labels = parse_pio(AUDIO / "inmp441_i2s.pio")
    assert len(program) <= 32                          # memória de instruções de uma PIO
    assert wrap == len(program) - 1 and wrap_target == 0
    assert program[labels["entry"]].op == "set"         # a entrada carrega X e baixa WS
    for instruction in program:
        if instruction.op == "in":
            assert instruction.side & 1 == 1           # lê com SCK alto (após a subida)
        else:
            assert instruction.side & 1 == 0 or instruction.op == "nop"


# ---------------------------------------------------------------------------
# 2. Contrato de código-fonte
# ---------------------------------------------------------------------------


def test_audio_firmware_builds_for_both_boards_and_reuses_the_link() -> None:
    cmake = (AUDIO / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "set(PICO_BOARD pico_w CACHE" in cmake
    assert "pico2_w" in cmake and "PICO_CYW43_SUPPORTED" in cmake
    for shared in ("edge_network_driver.c", "edge_ble_beacon.c", "edge_link.c", "edge_diagnostics.c"):
        assert f"${{EDGE_WIFI_DIR}}/{shared}" in cmake
    assert "ioc_sha256.c" in cmake and "ioc_link.c" in cmake
    assert "pico_generate_pio_header" in cmake and "pico_multicore" in cmake
    # Mesmos segredos de build do sensor de vibração, vazios por padrão (fail-closed).
    assert 'set(EDGE_LINK_DEVICE_KEY ""' in cmake and 'set(EDGE_SERIAL_ADMIN_TOKEN "0"' in cmake
    # O firmware acústico não compila os arquivos próprios do acelerômetro.
    for vibration_only in ("mpu6050_dma_driver.c", "dsp_pipeline.c", "config_validation.c", "edge_app_vibration.c"):
        assert vibration_only not in cmake


def test_shared_link_layer_has_no_sensor_specific_dependency() -> None:
    driver = (WIFI / "edge_network_driver.c").read_text(encoding="utf-8")
    beacon = (WIFI / "edge_ble_beacon.c").read_text(encoding="utf-8")
    assert "mpu6050" not in driver and "dsp_get_" not in driver
    assert "hello.profile = edge_app_profile_id();" in driver
    assert "edge_app_profile_id()" in beacon and "PROFILE_ACCELEROMETER" not in beacon
    vibration = (WIFI / "edge_app_vibration.c").read_text(encoding="utf-8")
    audio = (AUDIO / "audio_app.c").read_text(encoding="utf-8")
    assert "return (uint8_t)PROFILE_ACCELEROMETER;" in vibration
    assert "return (uint8_t)PROFILE_ACOUSTIC_MEMS;" in audio


def test_dsp_runs_on_core1_and_network_stays_on_core0() -> None:
    core1 = (AUDIO / "audio_core1.c").read_text(encoding="utf-8")
    main = (AUDIO / "main.c").read_text(encoding="utf-8")
    assert "multicore_launch_core1_with_stack" in core1
    assert "queue_try_add" in core1 and "queue_try_remove" in core1
    # O núcleo 1 não toca rede, console nem diagnóstico.
    for forbidden in ("edge_net_", "printf", "diag_report_dtc_event", "cyw43"):
        assert forbidden not in core1
    # O rádio reserva PIO/DMA antes do microfone, e o watchdog depende do núcleo 1.
    assert main.index("edge_net_init();") < main.index("audio_core1_start(&initial);")
    assert "audio_core1_heartbeat()" in main and "DTC_DSP_CORE_STALL" in main


def test_i2s_driver_claims_resources_dynamically_and_handles_dma_errata() -> None:
    driver = (AUDIO / "inmp441_i2s.c").read_text(encoding="utf-8")
    assert "pio_claim_free_sm_and_add_program_for_gpio_range" in driver
    assert "dma_claim_unused_channel" in driver
    assert "irq_add_shared_handler(DMA_IRQ_1" in driver
    assert "RP2040-E13" in driver and "channel_config_set_chain_to(&c, (uint)g_dma[i]);" in driver
    assert "GPIO_OVERRIDE_INVERT" in driver


def test_acoustic_profile_is_announced_and_defaults_depend_on_the_chip() -> None:
    config = (AUDIO / "audio_config.c").read_text(encoding="utf-8")
    assert "PICO_RP2350" in config
    assert "48000.0f" in config and "16000.0f" in config
    protocol = (WIFI / "edge_protocol_definitions.h").read_text(encoding="utf-8")
    assert "PROFILE_ACOUSTIC_MEMS   = 0x02" in protocol
    assert "CMD_ACOUSTIC_STREAM     = 0x21" in protocol
    assert "Payload_AcousticStream" in protocol


# ---------------------------------------------------------------------------
# 3. Testes nativos do DSP e da validação
# ---------------------------------------------------------------------------

needs_cc = pytest.mark.skipif(shutil.which("cc") is None, reason="compilador C indisponível")


@needs_cc
def test_native_acoustic_dsp_and_config(tmp_path: Path) -> None:
    dsp = tmp_path / "test_acoustic_dsp"
    subprocess.run(
        ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{AUDIO}", f"-I{WIFI}",
         str(AUDIO / "acoustic_dsp.c"), str(AUDIO / "audio_fft.c"), str(AUDIO / "tests" / "test_acoustic_dsp.c"),
         "-lm", "-o", str(dsp)],
        check=True,
    )
    result = subprocess.run([str(dsp)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stderr
    assert "PASS" in result.stdout
    cfg = tmp_path / "test_audio_config"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{AUDIO}", f"-I{WIFI}",
         str(AUDIO / "audio_config.c"), str(AUDIO / "tests" / "test_audio_config.c"), "-lm", "-o", str(cfg)],
        check=True,
    )
    result = subprocess.run([str(cfg)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stderr
