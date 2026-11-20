"""Scripts de compilação e gravação: chave obrigatória, pasta por variante e pinos do microfone.

Os scripts são executados de verdade, numa árvore mínima, com ``pio`` e ``cmake`` de mentira que só
registram como foram chamados.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
KEY = "ab" * 32


@pytest.fixture
def tree(tmp_path):
    scripts = tmp_path / "Codigo" / "scripts"
    scripts.mkdir(parents=True)
    for name in ("build_esp32_can_node.sh", "upload_esp32_can_node.sh", "build_pico.sh"):
        shutil.copy(ROOT / "Codigo" / "scripts" / name, scripts / name)
    for folder in ("node-can", "node-wifi", "node-audio"):
        (tmp_path / "Codigo" / folder).mkdir(parents=True)
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    log = tmp_path / "chamadas.log"
    # pio: registra o que o script entregou ao PlatformIO (id, chave e liberação sem chave).
    pio = bin_dir / "pio"
    pio.write_text(
        '#!/usr/bin/env bash\n'
        f'echo "pio $* | ID=$IOT_NODE_ID KEY=${{IOT_LINK_MASTER_KEY:-}} SEMCHAVE=${{IOT_ALLOW_NO_LINK_KEY:-}}" >> "{log}"\n'
    )
    # cmake: registra os argumentos e, no --build, cria os .uf2 na pasta atual.
    cmake = bin_dir / "cmake"
    cmake.write_text(
        '#!/usr/bin/env bash\n'
        'if [ "$1" = "--build" ]; then touch edge_node_firmware.uf2 edge_audio_firmware.uf2; exit 0; fi\n'
        f'echo "cmake in $(basename "$PWD") | $*" >> "{log}"\n'
    )
    for tool in (pio, cmake):
        tool.chmod(0o755)
    return tmp_path


def run(tree, script, *args, env_file=None, expect=0, **env):
    if env_file is not None:
        (tree / ".env.local").write_text(env_file)
    environment = {key: value for key, value in os.environ.items() if not key.startswith(("IOT_", "EDGE_"))}
    environment["PATH"] = f"{tree / 'bin'}:{environment['PATH']}"
    environment.update(env)
    result = subprocess.run(["bash", str(tree / "Codigo" / "scripts" / script), *args],
                            capture_output=True, text=True, env=environment, cwd=tree)
    assert result.returncode == expect, result.stdout + result.stderr
    return result


def calls(tree) -> list[str]:
    log = tree / "chamadas.log"
    return log.read_text().splitlines() if log.exists() else []


# --- módulos CAN ------------------------------------------------------------


@pytest.mark.parametrize("script", ["build_esp32_can_node.sh", "upload_esp32_can_node.sh"])
def test_a_module_without_link_key_is_not_built_unless_asked(tree, script):
    result = run(tree, script, "3", expect=3)
    assert "chave de enlace ausente" in result.stderr and "--sem-chave" in result.stderr
    assert calls(tree) == []          # o PlatformIO nem foi chamado

    run(tree, script, "3", "--sem-chave")
    (line,) = calls(tree)
    assert "ID=3" in line and "KEY= " in line and "SEMCHAVE=1" in line


@pytest.mark.parametrize("script", ["build_esp32_can_node.sh", "upload_esp32_can_node.sh"])
def test_a_module_with_link_key_is_built_with_it(tree, script):
    run(tree, script, "4", env_file=f"EDGE_LINK_MASTER_KEY={KEY}\n")
    (line,) = calls(tree)
    assert "ID=4" in line and f"KEY={KEY}" in line and "SEMCHAVE= " in line + " "


@pytest.mark.parametrize("script", ["build_esp32_can_node.sh", "upload_esp32_can_node.sh"])
def test_the_probe_does_not_need_a_key(tree, script):
    run(tree, script, "0")
    (line,) = calls(tree)
    assert "ID=0" in line


def test_upload_accepts_the_flag_before_or_after_the_port(tree):
    run(tree, "upload_esp32_can_node.sh", "--sem-chave", "2", "/dev/ttyUSB1")
    run(tree, "upload_esp32_can_node.sh", "2", "/dev/ttyUSB1", "--sem-chave")
    first, second = calls(tree)
    assert first == second and "--upload-port /dev/ttyUSB1" in first and "-t upload" in first


def test_node_id_must_be_valid(tree):
    run(tree, "build_esp32_can_node.sh", "32", "--sem-chave", expect=2)
    run(tree, "build_esp32_can_node.sh", "--sem-chave", expect=1)
    run(tree, "build_esp32_can_node.sh", "1", "--outra", expect=2)


def test_platformio_script_refuses_a_module_without_key():
    """Chamar o PlatformIO direto (botão do editor) também não compila um módulo sem chave por engano."""

    source = (ROOT / "Codigo" / "node-can" / "tools" / "node_id.py").read_text(encoding="utf-8")
    assert 'IOT_ALLOW_NO_LINK_KEY' in source and "raise ValueError" in source[source.index("IOT_ALLOW_NO_LINK_KEY"):]


# --- sensores Pico ------------------------------------------------------------


def test_a_sensor_without_link_key_is_not_built_unless_asked(tree):
    result = run(tree, "build_pico.sh", expect=3)
    assert "não há chave de enlace" in result.stderr and "--sem-chave" in result.stderr
    assert calls(tree) == []
    out = run(tree, "build_pico.sh", "--sem-chave").stdout
    assert "compilado com --sem-chave" in out
    assert (tree / "Codigo/node-wifi/build/pico_w/edge_node_firmware.uf2").exists()


def test_each_board_and_sensor_keeps_its_own_output_folder(tree):
    """Compilar para o Pico 2 W, ou para outro sensor, não apaga o .uf2 do Pico W."""

    run(tree, "build_pico.sh", env_file=f"EDGE_LINK_MASTER_KEY={KEY}\n")
    run(tree, "build_pico.sh", "pico2_w")
    run(tree, "build_pico.sh", "pico_w", "--audio")
    (tree / ".env.sensor-E6616408432B6F39.local").write_text(
        f"EDGE_LINK_DEVICE_KEY={KEY}\nEDGE_LINK_NETWORK_KEY={KEY}\nEDGE_LINK_DEVICE_UUID=E6616408432B6F39\n")
    run(tree, "build_pico.sh", "--sensor", "0xe6616408432b6f39")
    base = tree / "Codigo"
    assert (base / "node-wifi/build/pico_w/edge_node_firmware.uf2").exists()
    assert (base / "node-wifi/build/pico2_w/edge_node_firmware.uf2").exists()
    assert (base / "node-wifi/build/pico_w-E6616408432B6F39/edge_node_firmware.uf2").exists()
    assert (base / "node-audio/build/pico_w/edge_audio_firmware.uf2").exists()
    # a configuração do CMake aponta para o código-fonte e entra na pasta da variante
    assert any(line.startswith("cmake in pico2_w |") and f"{base / 'node-wifi'}" in line for line in calls(tree))


def test_microphone_pins_reach_cmake_and_name_the_folder(tree):
    run(tree, "build_pico.sh", "pico2_w", "--audio", "--mic-pins", "10,12", env_file=f"EDGE_LINK_MASTER_KEY={KEY}\n")
    (line,) = [item for item in calls(tree) if item.startswith("cmake in")]
    assert line.startswith("cmake in pico2_w-mic10-12 |")
    assert "-DAUDIO_PIN_SCK=10" in line and "-DAUDIO_PIN_SD=12" in line
    assert (tree / "Codigo/node-audio/build/pico2_w-mic10-12/edge_audio_firmware.uf2").exists()


@pytest.mark.parametrize(
    "pins, message",
    [("18", "espera <SCK>,<SD>"), ("a,b", "espera <SCK>,<SD>"), ("23,12", "GPIO 23"), ("22,12", "GPIO 23"),
     ("28,12", "GPIO 29"), ("10,10", "SD não pode"), ("10,11", "SD não pode")],
)
def test_invalid_microphone_pins_are_refused(tree, pins, message):
    result = run(tree, "build_pico.sh", "--audio", "--mic-pins", pins, "--sem-chave", expect=2)
    assert message in result.stderr and calls(tree) == []


def test_microphone_pins_need_the_audio_sensor(tree):
    result = run(tree, "build_pico.sh", "--mic-pins", "10,12", "--sem-chave", expect=2)
    assert "só vale com --audio" in result.stderr


# --- esperas do sensor na reassociação ---------------------------------------


def test_sensor_waits_reach_cmake_and_name_the_folder(tree):
    run(tree, "build_pico.sh", "--espera-sessao", "9000", "--espera-oferta", "5000",
        env_file=f"EDGE_LINK_MASTER_KEY={KEY}\n")
    (line,) = [item for item in calls(tree) if item.startswith("cmake in")]
    assert line.startswith("cmake in pico_w-sess9000-oferta5000 |")
    assert "-DIOT_SESSION_TIMEOUT_MS=9000" in line and "-DIOT_OFFER_SWITCH_HOLD_MS=5000" in line
    assert (tree / "Codigo/node-wifi/build/pico_w-sess9000-oferta5000/edge_node_firmware.uf2").exists()


def test_without_the_options_the_firmware_defaults_are_untouched(tree):
    """Sem as opções nenhum -D de espera é passado: o padrão é o do edge_protocol_definitions.h."""

    out = run(tree, "build_pico.sh", env_file=f"EDGE_LINK_MASTER_KEY={KEY}\n").stdout
    (line,) = [item for item in calls(tree) if item.startswith("cmake in")]
    assert "IOT_SESSION_TIMEOUT_MS" not in line and "IOT_OFFER_SWITCH_HOLD_MS" not in line
    assert line.startswith("cmake in pico_w |")
    assert "Esperas:" not in out


def test_only_one_wait_can_be_changed(tree):
    run(tree, "build_pico.sh", "--audio", "--espera-oferta", "8000", env_file=f"EDGE_LINK_MASTER_KEY={KEY}\n")
    (line,) = [item for item in calls(tree) if item.startswith("cmake in")]
    assert line.startswith("cmake in pico_w-oferta8000 |")
    assert "-DIOT_OFFER_SWITCH_HOLD_MS=8000" in line and "IOT_SESSION_TIMEOUT_MS" not in line


@pytest.mark.parametrize(
    "option, value",
    [("--espera-sessao", "6999"), ("--espera-sessao", "60001"), ("--espera-sessao", "dez"),
     ("--espera-oferta", "999"), ("--espera-oferta", "120001"), ("--espera-oferta", "-5")],
)
def test_waits_outside_the_checked_range_are_refused(tree, option, value):
    result = run(tree, "build_pico.sh", option, value, "--sem-chave", expect=2)
    assert option in result.stderr and "ms" in result.stderr and calls(tree) == []


def test_the_firmware_header_checks_the_same_ranges():
    """O script e o cabeçalho têm de concordar: o cabeçalho é quem derruba uma compilação fora da faixa."""

    import shutil as _shutil
    if _shutil.which("cc") is None:
        pytest.skip("compilador C indisponível")
    header = ROOT / "Codigo" / "node-wifi"

    def compiles(*flags):
        result = subprocess.run(["cc", "-std=c11", "-fsyntax-only", "-x", "c", "-", f"-I{header}", *flags],
                                input='#include "edge_protocol_definitions.h"\nint main(void){return NET_SESSION_TIMEOUT_MS;}\n',
                                capture_output=True, text=True)
        return result.returncode == 0

    assert compiles()
    assert compiles("-DIOT_SESSION_TIMEOUT_MS=7000", "-DIOT_OFFER_SWITCH_HOLD_MS=1000")
    assert compiles("-DIOT_SESSION_TIMEOUT_MS=60000", "-DIOT_OFFER_SWITCH_HOLD_MS=120000")
    assert not compiles("-DIOT_SESSION_TIMEOUT_MS=6999")      # um CMD_PING perdido derrubaria a sessão
    assert not compiles("-DIOT_SESSION_TIMEOUT_MS=60001")
    assert not compiles("-DIOT_OFFER_SWITCH_HOLD_MS=999")
    assert not compiles("-DIOT_OFFER_SWITCH_HOLD_MS=120001")
