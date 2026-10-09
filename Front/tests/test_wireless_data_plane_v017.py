"""Plano de dados wireless e autenticação do enlace (v0.17).

Três níveis de verificação:

1. contrato de código-fonte dos firmwares (o que precisa existir e o que foi
   removido);
2. conferência cruzada da biblioteca C ``ioc_link`` com ``hmac``/``hashlib``;
3. ponta a ponta: a simulação ``sim_wireless_link`` executa o código real do
   Node CAN e da Probe 00 contra um sensor simulado, e as linhas produzidas
   pela Probe são entregues ao decodificador e ao domínio reais da TUI.

Os níveis 2 e 3 precisam de compilador C/C++; sem ele são ignorados (a
integração contínua sempre os executa).
"""
from __future__ import annotations

import asyncio
import hashlib
import hmac
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from pico_tui import commands
from pico_tui.command_catalog import actions_for
from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    CommandAck,
    DtcReceived,
    SpectrumReceived,
    TelemetryReceived,
    WirelessConfigReceived,
    WirelessLinkReceived,
)
from pico_tui.core.models import SensorMode, SensorNode
from pico_tui.core.state_store import StateStore
from pico_tui.protocol.router import DecoderRouter
from pico_tui.security import SecurityManager
from pico_tui.services.controller import DomainController

ROOT = Path(__file__).resolve().parents[2]
CODE = ROOT / "Codigo"
LINK = CODE / "common" / "ioc_link"
UUID = "0xE6616408432B6F39"
ASSOC_LINE = f"[GW] WIRELESS_ASSOC node=1 child=1 uuid={UUID} profile=VIBRATION state=ONLINE rssi=-48 protocol=6"
LINK_SECURE = (
    "[GW] WIRELESS_LINK node=1 child=1 state=SECURE rssi=-47 age_s=3 rx=10 "
    "auth_fail=0 replay=0 lost=0 stream=ON period_ms=1000"
)

needs_cc = pytest.mark.skipif(shutil.which("cc") is None, reason="compilador C indisponível")
needs_cxx = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("c++") is None, reason="compilador C/C++ indisponível"
)


# --------------------------------------------------------------------------
# 1. Contrato de código-fonte
# --------------------------------------------------------------------------


def test_can_identifiers_reserve_lowest_priority_range_for_sensor_data() -> None:
    ids = (CODE / "node-can/include/can_ids.h").read_text()
    assert "#define CAN_ID_WIRELESS_DATA_CMD          0x304" in ids
    assert "#define CAN_ID_WIRELESS_DATA_BASE         0x380" in ids
    assert "#define CAN_ID_WIRELESS_DATA_LAST         0x39F" in ids
    # Dados de sensores usam os maiores identificadores do projeto: nunca
    # vencem a arbitragem contra eleição, controle, liveness ou associação.
    values = [int(token, 16) for token in re.findall(r"0x[0-9A-Fa-f]{3}\b", ids)]
    # A única exceção é a carga artificial de baixa prioridade dos ensaios
    # (v0.18), gerada pela Probe 00 e ignorada pelos Nodes.
    assert "#define CAN_ID_TEST_LOAD_LOW              0x7EF" in ids
    assert max(value for value in values if value != 0x7EF) == 0x39F


def test_sensor_firmware_shares_link_library_and_scans_for_offers() -> None:
    cmake = (CODE / "node-wifi/CMakeLists.txt").read_text()
    beacon = (CODE / "node-wifi/edge_ble_beacon.c").read_text()
    btstack = (CODE / "node-wifi/btstack_config.h").read_text()
    driver = (CODE / "node-wifi/edge_network_driver.c").read_text()
    assert "../common/ioc_link" in cmake
    assert "ioc_sha256.c" in cmake and "ioc_link.c" in cmake
    assert "pico_rand" in cmake
    assert "#define ENABLE_LE_CENTRAL" in btstack
    assert "GAP_EVENT_ADVERTISING_REPORT" in beacon
    assert "ioc_offer_verify(edge_link_device_key()" in driver
    # A oferta só é considerada quando endereçada a este UUID.
    assert "offer.uuid != edge_device_uuid64()" in driver
    # O CHALLENGE é autenticado antes de qualquer mudança de estado.
    handler = driver[driver.index("static void handle_challenge") :]
    assert handler.index("ioc_env_verify(edge_link_device_key()") < handler.index("g_net_mode = NET_STATE_BOUND")


def test_sensor_never_stores_master_key_in_recommended_build() -> None:
    link = (CODE / "node-wifi/edge_link.c").read_text()
    build = (CODE / "scripts/build_pico.sh").read_text()
    provision = (CODE / "scripts/provision_sensor_security.sh").read_text()
    assert "EDGE_LINK_KEYS_UUID_MISMATCH" in link
    assert "ioc_secure_zero(master" in link
    # Com --sensor, o build recebe apenas chaves derivadas.
    sensor_branch = build[build.index('if [ -n "$SENSOR_UUID" ]') : build.index("else", build.index('if [ -n "$SENSOR_UUID" ]'))]
    assert "EDGE_LINK_DEVICE_KEY" in sensor_branch and "EDGE_LINK_MASTER_KEY" not in sensor_branch
    assert "--sensor" in provision and "ioc-dev-v1" in provision
    assert "EDGE_NODE_PRESHARED_KEY=$" not in provision


def test_can_node_bridges_only_bound_and_authenticated_sensors() -> None:
    link = (CODE / "node-can/src/wireless_link.cpp").read_text()
    discovery = (CODE / "node-can/src/wireless_discovery.cpp").read_text()
    platformio = (CODE / "node-can/platformio.ini").read_text()
    main = (CODE / "node-can/src/main.ino").read_text()
    assert "wirelessLinkOnBound(uuid, childId);" in discovery
    assert "wirelessLinkOnUnbound(uuid, snapshot.childId);" in discovery
    assert "wirelessLinkHandleCanMessage(rx)" in main and "wirelessLinkPoll();" in main
    assert "symlink://../common/ioc_link" in platformio
    assert "board_build.partitions = min_spiffs.csv" in platformio
    # HELLO de UUID sem vínculo não recebe resposta.
    hello = link[link.index("void handleHello") : link.index("// Promove o handshake pendente")]
    assert hello.index("findLinkByUuid(hello.uuid)") < hello.index("sendEnvelope(")
    assert hello.index("ioc_env_verify(link->devKey") < hello.index("sendEnvelope(")
    # A Probe 00 não mantém sessão com sensores nem conhece a chave.
    init = link[link.index("void wirelessLinkInit()") :]
    assert init.index("if (NODE_ID == 0)") < init.index("IOT_LINK_MASTER_KEY_HEX")
    assert 'Serial.printf("[NODE %u] [WLINK] LINK_KEY=%s' in link
    assert "masterKey" not in "".join(line for line in link.splitlines() if "printf" in line)


def test_tui_commands_for_wireless_children_use_probe_cmd_syntax_and_are_protected(tmp_path: Path) -> None:
    line = commands.gateway_command("01.01", "FFT", tx="A00002", MODE="VIEW_ONLY", BINS=64)
    # Mesma linha usada na simulação de ponta a ponta.
    assert line == "CMD TARGET=01.01 ACTION=FFT TX=A00002 MODE=VIEW_ONLY BINS=64"
    security = SecurityManager("off", tmp_path / "missing.json")
    assert security.command_requires_auth(line) is True
    assert security.command_requires_auth("CMD TARGET=01.01 ACTION=CONFIG TX=1 MODE=IDLE") is True


def test_actions_without_route_through_node_are_not_offered_in_probe_mode() -> None:
    via_probe = {action.key for action in actions_for("sensor", profile="VIBRATION", mode="GATEWAY_CAN")}
    direct = {action.key for action in actions_for("sensor", profile="VIBRATION", mode="SENSOR_DIRECT")}
    for key in ("sensor.restart_acq", "sensor.wifi_on", "sensor.wifi_off", "sensor.dtc_refresh"):
        assert key in direct and key not in via_probe
    for key in ("sensor.fft", "sensor.configure", "sensor.telemetry_period", "sensor.dtc_clear"):
        assert key in via_probe


# --------------------------------------------------------------------------
# 2. Biblioteca C x Python
# --------------------------------------------------------------------------


def _mac(key: bytes, message: bytes) -> bytes:
    return hmac.new(key, message, hashlib.sha256).digest()


@needs_cc
def test_c_link_library_matches_python_reference(tmp_path: Path) -> None:
    binary = tmp_path / "test_ioc_link"
    subprocess.run(
        [
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{LINK}",
            str(LINK / "ioc_sha256.c"), str(LINK / "ioc_link.c"), str(LINK / "ioc_wdata.c"),
            str(CODE / "common/tests/test_ioc_link.c"), "-lm", "-o", str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True, capture_output=True)
    output = subprocess.run([str(binary), "--vectors"], check=True, capture_output=True, text=True).stdout
    vectors = dict(line.split("=", 1) for line in output.splitlines())

    master = bytes(range(32))
    uuid = bytes.fromhex(UUID[2:])
    device = _mac(master, b"ioc-dev-v1" + uuid)
    network = _mac(master, b"ioc-net-v1")
    sensor_nonce = bytes(range(1, 9))
    node_nonce = bytes(range(0xA0, 0xA8))
    session = _mac(device, b"ioc-sess-v1" + uuid + bytes([3]) + sensor_nonce + node_nonce)
    signed = b"IO" + bytes([1, 3]) + uuid + bytes([1]) + (0x01020304).to_bytes(4, "big")
    header = b"IL" + bytes([6, 0x10]) + (0x01020304).to_bytes(4, "little") + bytes.fromhex("deadbeef01")

    assert vectors["device_key"] == device.hex()
    assert vectors["network_key"] == network.hex()
    assert vectors["wifi_psk_node3"] == _mac(network, b"ioc-psk-v1" + bytes([3])).hex()[:32]
    assert vectors["session_key"] == session.hex()
    assert vectors["offer"] == (b"\xff\xff" + signed + _mac(device, b"ioc-offer-v1" + signed)[:8]).hex()
    assert vectors["envelope"] == (header + _mac(session, header)[:8]).hex()
    assert vectors["crc8"] == "f4"  # CRC-8 polinômio 0x07 de "123456789"


def test_provisioning_script_derives_the_same_device_key(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    (repo / "Codigo").mkdir(parents=True)
    shutil.copytree(CODE / "scripts", repo / "Codigo" / "scripts")
    env = {"HOME": str(tmp_path), "XDG_CONFIG_HOME": str(tmp_path / "config"), "PATH": "/usr/bin:/bin:/usr/local/bin"}
    script = repo / "Codigo/scripts/provision_sensor_security.sh"

    subprocess.run([str(script)], check=True, env=env, capture_output=True)
    result = subprocess.run([str(script), "--sensor", UUID], check=True, env=env, capture_output=True, text=True)
    subprocess.run([str(script), "--check"], check=True, env=env, capture_output=True)

    local = dict(line.split("=", 1) for line in (repo / ".env.local").read_text().splitlines())
    sensor_file = repo / f".env.sensor-{UUID[2:]}.local"
    sensor = dict(line.split("=", 1) for line in sensor_file.read_text().splitlines())
    master = bytes.fromhex(local["EDGE_LINK_MASTER_KEY"])

    assert sensor["EDGE_LINK_DEVICE_KEY"] == _mac(master, b"ioc-dev-v1" + bytes.fromhex(UUID[2:])).hex()
    assert sensor["EDGE_LINK_NETWORK_KEY"] == _mac(master, b"ioc-net-v1").hex()
    assert sensor["EDGE_LINK_DEVICE_UUID"] == UUID[2:]
    assert "EDGE_LINK_MASTER_KEY" not in sensor
    assert sensor_file.stat().st_mode & 0o777 == 0o600
    assert (repo / ".env.local").stat().st_mode & 0o777 == 0o600
    # Nenhum segredo aparece na saída do script.
    for secret in (local["EDGE_LINK_MASTER_KEY"], sensor["EDGE_LINK_DEVICE_KEY"], sensor["EDGE_LINK_NETWORK_KEY"]):
        assert secret not in result.stdout and secret not in result.stderr


# --------------------------------------------------------------------------
# 3. Domínio da TUI
# --------------------------------------------------------------------------


def _new_domain() -> tuple[EventBus, StateStore, DecoderRouter, dict[type, list]]:
    bus = EventBus()
    state = StateStore()
    DomainController(bus, state)
    router = DecoderRouter(bus, "gateway")
    seen: dict[type, list] = {}
    for event_type in (
        TelemetryReceived, SpectrumReceived, CommandAck, DtcReceived, WirelessLinkReceived, WirelessConfigReceived,
    ):
        seen[event_type] = []

        async def collect(event, bucket=seen[event_type]) -> None:
            bucket.append(event)

        bus.subscribe(event_type, collect)
    return bus, state, router, seen


def test_data_plane_follows_link_state_reported_by_node() -> None:
    async def scenario() -> None:
        _, state, router, _ = _new_domain()
        await router.decode(ASSOC_LINE)
        sensor = state.snapshot().nodes[1].sensors[1]
        assert sensor.wireless_associated and not sensor.has_data_plane
        assert sensor.data_link_state == "NONE"

        await router.decode(LINK_SECURE.replace("SECURE rssi=-47", "OFFERING rssi=0"))
        assert state.snapshot().nodes[1].sensors[1].has_data_plane is False

        await router.decode(LINK_SECURE)
        sensor = state.snapshot().nodes[1].sensors[1]
        assert sensor.has_data_plane
        assert sensor.data_link_rssi_dbm == -47
        assert sensor.health.telemetry_enabled is True
        assert sensor.health.telemetry_period_ms == 1000

        # A sessão cai: as ações voltam a ficar indisponíveis e o operador é avisado.
        await router.decode(LINK_SECURE.replace("SECURE rssi=-47", "OFFERING rssi=0"))
        sensor = state.snapshot().nodes[1].sensors[1]
        assert not sensor.has_data_plane
        assert sensor.health.telemetry_enabled is False
        assert any("sessão de dados encerrada" in incident.message for incident in sensor.incidents)

    asyncio.run(scenario())


def test_rejected_datagrams_become_security_incident() -> None:
    async def scenario() -> None:
        _, state, router, _ = _new_domain()
        await router.decode(ASSOC_LINE)
        await router.decode(LINK_SECURE)
        await router.decode(LINK_SECURE.replace("auth_fail=0 replay=0", "auth_fail=2 replay=1"))
        sensor = state.snapshot().nodes[1].sensors[1]
        assert sensor.data_link_auth_failures == 2 and sensor.data_link_replay_drops == 1
        assert any("3 datagrama(s) rejeitado(s)" in incident.message for incident in sensor.incidents)
        # Sem crescimento dos contadores não há novo aviso.
        count = len(sensor.incidents)
        await router.decode(LINK_SECURE.replace("auth_fail=0 replay=0", "auth_fail=2 replay=1"))
        assert len(state.snapshot().nodes[1].sensors[1].incidents) == count

    asyncio.run(scenario())


def test_link_report_does_not_resurrect_unbound_child() -> None:
    async def scenario() -> None:
        _, state, router, _ = _new_domain()
        await router.decode(ASSOC_LINE)
        await router.decode(ASSOC_LINE.replace("state=ONLINE", "state=UNBOUND"))
        await router.decode(LINK_SECURE.replace("SECURE rssi=-47", "DOWN rssi=0"))
        assert 1 not in state.snapshot().nodes[1].sensors

    asyncio.run(scenario())


def test_no_key_state_is_visible_to_operator() -> None:
    async def scenario() -> None:
        _, state, router, _ = _new_domain()
        await router.decode(ASSOC_LINE)
        await router.decode(LINK_SECURE.replace("SECURE rssi=-47", "NO_KEY rssi=0"))
        sensor = state.snapshot().nodes[1].sensors[1]
        assert sensor.data_link_state == "NO_KEY" and not sensor.has_data_plane
        from pico_tui import presentation as pres

        assert "sem chave de enlace" in " ".join(pres.sensor_condition(sensor).reasons).lower()

    asyncio.run(scenario())


def test_model_defaults_keep_direct_and_demo_sensors_unaffected() -> None:
    sensor = SensorNode(1, 1)
    assert sensor.wireless_associated is False
    assert sensor.has_data_plane is False


# --------------------------------------------------------------------------
# 4. Ponta a ponta: firmware simulado -> texto da Probe -> TUI
# --------------------------------------------------------------------------


@pytest.fixture(scope="module")
def probe_lines(tmp_path_factory: pytest.TempPathFactory) -> list[str]:
    binary = tmp_path_factory.mktemp("sim") / "sim_wireless_link"
    subprocess.run([str(CODE / "scripts/build_wireless_link_sim.sh"), str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stderr
    return result.stdout.splitlines()


@needs_cxx
def test_end_to_end_probe_output_drives_tui_state(probe_lines: list[str]) -> None:
    async def scenario() -> None:
        _, state, router, seen = _new_domain()
        await router.decode(ASSOC_LINE)

        secure_seen = False
        checked_config = False
        for line in probe_lines:
            await router.decode(line)
            sensor = state.snapshot().nodes[1].sensors[1]
            if "state=SECURE" in line and not secure_seen:
                secure_seen = True
                assert sensor.has_data_plane
            if "WIRELESS_CONFIG" in line and "status=APPLIED" in line and "mode=STRUCTURAL" in line:
                checked_config = True
                cfg = sensor.configuration
                assert cfg.mode == SensorMode.STRUCTURAL
                assert cfg.sample_rate_requested_hz == 500.0
                assert cfg.window_type == "HAMMING" and cfg.window_size == 256
                assert cfg.stalta_threshold == 3.5 and cfg.calibration_gain == 1.25
                assert cfg.transaction_state == "APPLIED"
        assert secure_seen and checked_config

        # Telemetria com os valores enviados pelo sensor simulado.
        samples = [event.sample for event in seen[TelemetryReceived]]
        assert len(samples) >= 3
        first = samples[0]
        assert (first.parent_node_id, first.child_id, first.sequence) == (1, 1, 0)
        assert first.mode == SensorMode.ROTATING
        assert first.rms == pytest.approx(0.1242) and first.rms_unit == "m/s²"
        assert first.peak_frequency_hz == pytest.approx(31.25)
        assert first.window_size == 512 and first.fft_valid is True
        assert first.battery.percentage == 93 and first.battery.voltage_v == pytest.approx(3.98)

        # Espectros: 64 faixas em uma transferência e 256 em quatro.
        spectra = [event.sample for event in seen[SpectrumReceived]]
        assert [len(s.magnitudes) for s in spectra] == [64, 256]
        full = spectra[1]
        assert full.sample_rate_hz == 1000.0 and full.fft_size == 512 and full.window_type == "HANN"
        peak_index = max(range(256), key=full.magnitudes.__getitem__)
        assert peak_index == 16                       # pico gerado pelo sensor simulado
        assert full.magnitudes[16] == pytest.approx(1002.0, rel=1e-4)
        assert full.magnitudes[200] == pytest.approx(2.0, rel=0.01)
        reduced = spectra[0]
        assert max(range(64), key=reduced.magnitudes.__getitem__) == 4  # 16 // (256 / 64)

        # Confirmações de comando chegam com a transação original da TUI.
        acks = {(a.command, a.state, a.transaction_id) for a in seen[CommandAck]}
        assert ("FFT", "REJECTED", "A00001") in acks          # sem sessão
        assert ("FFT", "APPLIED", "A00002") in acks
        assert ("CONFIG", "APPLIED", "A00004") in acks
        assert ("CONFIG", "REJECTED", "A00005") in acks       # taxa inválida recusada pelo sensor
        assert ("DTC_CLEAR", "APPLIED", "A00008") in acks
        assert ("ACQ", "REJECTED", "A00009") in acks          # sem rota via Node
        assert ("CONFIG", "REJECTED", "A0000D") in acks       # configuração do sensor ainda desconhecida
        assert ("CONFIG", "APPLIED", "A0000F") in acks        # depois de o Node obter o menu

        # DTC urgente e estatísticas de segurança.
        assert [(e.parent_node_id, e.child_id, e.record.code) for e in seen[DtcReceived]] == [(1, 1, 0x2002)]
        links = seen[WirelessLinkReceived]
        assert max(event.auth_failures for event in links) == 2
        assert max(event.replay_drops for event in links) == 1
        assert links[-1].state == "DOWN"
        sensor = state.snapshot().nodes[1].sensors[1]
        assert not sensor.has_data_plane
        assert any(incident.kind == "SEGURANCA" for incident in sensor.incidents)

    asyncio.run(scenario())


@needs_cxx
def test_simulated_attacks_never_reach_the_can_bus(probe_lines: list[str]) -> None:
    # Na simulação, a telemetria repetida e a adulterada são enviadas depois da
    # amostra de sequência 6; nenhuma das duas pode aparecer na saída da Probe.
    sequences = [
        int(line.split("SEQ=")[1].split()[0]) for line in probe_lines if line.startswith("TEL NODE=1 CHILD=1")
    ]
    assert sequences == sorted(set(sequences)), "telemetria repetida chegou ao CAN"
    assert 7 not in sequences  # amostra com um bit alterado
