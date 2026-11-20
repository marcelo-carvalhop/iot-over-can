"""Verificação de fumaça depois de gravar uma placa (Codigo/scripts/smoke_boot.py).

Três camadas: o que o script espera de cada placa é conferido contra o código-fonte do firmware (as
linhas não podem mudar sem o script saber); o script reprova cada defeito conhecido com a dica certa; e a
mesma verificação roda sobre a saída do firmware real, compilado na bancada virtual.
"""
from __future__ import annotations

import dataclasses
import importlib.util
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Codigo" / "scripts" / "smoke_boot.py"
REPLAYS = ROOT / "Front" / "replays"

_spec = importlib.util.spec_from_file_location("smoke_boot", SCRIPT)
smoke = importlib.util.module_from_spec(_spec)
sys.modules["smoke_boot"] = smoke
_spec.loader.exec_module(smoke)

needs_cxx = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("c++") is None, reason="compilador C/C++ indisponível"
)

VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
COMMIT = "ab12cd34"
FEATURES = ",".join(smoke.FEATURES)
UUID = "0xE6616408432B6F39"


# --------------------------------------------------------------------------- saídas boas de cada placa


def probe_lines(version=VERSION, commit=COMMIT):
    return f"""
========================================
Iniciando NODE 0
========================================
[NODE 0] [BOOT] FIRMWARE={version} COMMIT={commit} PROTOCOL=CAN_CLASSIC_V1 ROLE=PROBE
[LEDS] IDLE: todos acesos. Logica invertida: LOW=ligado, HIGH=desligado
[PROBE 00] CAPS=CAN_MONITOR,SERIAL_INSTRUMENTATION LOCAL_PROFILE=NONE
[GW] Probe 00 ativada
[GW] Instrumentacao externa: nao participa da eleicao, TDMA ou sensoriamento
[GW] Interface serial da Probe 00 pronta
[PROBE 00] FIRMWARE={version} COMMIT={commit} FEATURES={FEATURES}
PROBE_VERSION FIRMWARE={version} COMMIT={commit} PROTOCOL=CAN_CLASSIC_V1 NODE=0 FEATURES={FEATURES}
PROBE_STATUS NODE=0 STATE=ONLINE CAN=CLASSIC ARB=500000 DATA=0 WIFI=OFF BLE_SCAN=OFF UPTIME_MS=4321
""".strip().splitlines()


def module_lines(node=2, version=VERSION, commit=COMMIT):
    return f"""
========================================
Iniciando NODE {node}
========================================
[NODE {node}] [BOOT] FIRMWARE={version} COMMIT={commit} PROTOCOL=CAN_CLASSIC_V1 ROLE=NODE
[LEDS] IDLE: todos acesos. Logica invertida: LOW=ligado, HIGH=desligado
[NODE {node}] CAPS=CAN,LOCAL_SENSOR,LOCAL_SENSOR_DEMO,BLE_SCAN LOCAL_PROFILE=DEMO_BYTE
[NODE {node}] BLE_SCAN=ACTIVE
[NODE {node}] [WLINK] LINK_KEY=PROVISIONED
[NODE {node}] [INIT] Pronto. Aguardando eleicao manual ou heartbeat para JOIN
NODE_VERSION NODE={node} FIRMWARE={version} COMMIT={commit} PROTOCOL=CAN_CLASSIC_V1 FEATURES={FEATURES}
""".strip().splitlines()


def vibration_lines(version=VERSION, commit=COMMIT):
    return f"""
BOOT FIRMWARE={version} COMMIT={commit} PROTOCOL=6 PROFILE=VIBRATION
VERSION FIRMWARE={version} COMMIT={commit} PROTOCOL=6 NODE_UUID={UUID}
STATUS NET=DISABLED MODE=ROTATING ACQ=CONTINUOUS WINDOW=HANN WINDOW_SIZE=256 RATE_HZ=1000.00 STALTA=3.000 GAIN=1.000 DTC=0x0000 DTC_COUNT=0 MPU=YES SIM=NO TELEMETRY=ON PERIOD_MS=1000 BATT_PCT=100 BATT_MV=4100 DRDY_IRQ=1 DRDY_MISSED=0 AUTH=LOCKED WIFI_PROVISIONED=NO BLE_BEACON=ON
NET STATE=DISABLED WIFI=OFF PROVISIONED=NO BLE_BEACON=ON LINK_KEYS=MASTER PARENT_NODE=0 CHILD=0 AUTH_REJECT=0 REPLAY_REJECT=0
""".strip().splitlines()


def acoustic_lines(version=VERSION, commit=COMMIT, mic="OK", coverage=100):
    return f"""
BOOT FIRMWARE={version} COMMIT={commit} PROTOCOL=6 PROFILE=ACOUSTIC CHIP=RP2040
VERSION FIRMWARE={version} COMMIT={commit} PROTOCOL=6 NODE_UUID={UUID} PROFILE=ACOUSTIC CHIP=RP2040 BOARD=pico_w
STATUS PROFILE=ACOUSTIC NET=DISABLED MODE=ROTATING ACQ=I2S RATE_HZ=16000.00 RATE_EFF_HZ=16000.00 WINDOW=HANN WINDOW_SIZE=512 THRESHOLD=3.000 STALTA=3.000 GAIN=1.0000 CHANNEL=LEFT MIC={mic} CPU_PCT=41 COVERAGE_PCT={coverage} CHIP=RP2040 BOARD=pico_w DTC=0x0000 DTC_COUNT=0 TELEMETRY=ON PERIOD_MS=1000 BATT_PCT=100 BATT_MV=4100 AUTH=LOCKED WIFI_PROVISIONED=NO BLE_BEACON=ON
NET STATE=DISABLED WIFI=OFF PROVISIONED=NO BLE_BEACON=ON LINK_KEYS=DEVICE PARENT_NODE=0 CHILD=0 AUTH_REJECT=0 REPLAY_REJECT=0
""".strip().splitlines()


def panel_lines(version=VERSION, commit=COMMIT):
    return [f"#IOC READY uuid=0xE66164084327A12C profile=PANEL fw={version} commit={commit}"]


def reader_lines(version=VERSION, commit=COMMIT):
    return [f"#LEITOR firmware {version} commit {commit}", "#LEITOR documentação 0.22.0, 9 documentos; tela 800x600"]


def run(role, lines, node=None, selftest=False, **kwargs):
    plan = smoke.build_plan(role, node, selftest)
    kwargs.setdefault("expected_version", VERSION)
    return smoke.evaluate(plan, [smoke.normalize(line) for line in lines], **kwargs)


def failed(report):
    return {item.name for item in report.results if item.level == smoke.FAIL}


def warned(report):
    return {item.name for item in report.results if item.level == smoke.WARN}


def hint_of(report, name):
    return next(item.hint for item in report.results if item.name == name)


GOOD = {
    "probe": lambda: run("probe", probe_lines()),
    "modulo": lambda: run("modulo", module_lines(2), node=2),
    "vibracao": lambda: run("vibracao", vibration_lines()),
    "acustico": lambda: run("acustico", acoustic_lines()),
    "painel": lambda: run("painel", panel_lines()),
    "leitor": lambda: run("leitor", reader_lines()),
}


# --------------------------------------------------------------------------- o que o script espera é o que o firmware escreve


def all_plans():
    for role in smoke.ROLES:
        yield smoke.build_plan(role, 2 if role == "modulo" else None)
    yield smoke.build_plan("modulo", 2, selftest=True)
    yield smoke.build_plan("probe", None, selftest=True)


def test_every_expected_line_is_written_by_the_firmware_source() -> None:
    """Se alguém muda uma dessas linhas no firmware, o script precisa mudar junto: este teste acusa."""

    checked = 0
    for plan in all_plans():
        for check in plan.checks:
            if check.source is None:
                continue
            path, fragment = check.source
            assert fragment in (ROOT / path).read_text(encoding="utf-8"), f"{plan.role}/{check.name}: {path} não tem {fragment!r}"
            checked += 1
    assert checked >= 40


def test_every_check_has_a_source_and_every_rule_a_hint() -> None:
    for plan in all_plans():
        for check in plan.checks:
            assert check.source is not None, f"{plan.role}/{check.name} sem fonte no firmware"
            assert check.hint
        for rule in plan.forbidden:
            assert rule.hint
        names = {check.name for check in plan.checks}
        for rule in plan.forbidden:
            assert not rule.explains or rule.explains in names, f"{plan.role}/{rule.name} explica uma verificação que não existe"


def test_feature_list_matches_the_firmware_header() -> None:
    header = (ROOT / "Codigo/node-can/include/firmware_info.h").read_text(encoding="utf-8")
    declared = re.search(r'#define IOT_FEATURES "([^"]+)"', header).group(1).split(",")
    assert tuple(declared) == smoke.FEATURES
    assert re.search(r'#define IOT_PROTOCOL_NAME "([^"]+)"', header).group(1) == smoke.PROTOCOL


# --------------------------------------------------------------------------- saídas boas passam


@pytest.mark.parametrize("role", smoke.ROLES)
def test_a_healthy_board_passes(role) -> None:
    report = GOOD[role]()
    assert report.verdict == "PASS", [(item.name, item.level, item.detail) for item in report.results if item.level != smoke.OK]
    assert report.firmware == VERSION and report.commit == COMMIT


def test_the_report_ends_with_one_line_for_scripts() -> None:
    text = smoke.format_report(GOOD["modulo"](), "/dev/ttyUSB1")
    last = text.splitlines()[-1]
    assert re.fullmatch(rf"SMOKE RESULT=PASS ROLE=modulo CHECKS=\d+ WARN=0 FAIL=0 FIRMWARE={re.escape(VERSION)} COMMIT={COMMIT}", last)
    assert "== Verificação de fumaça: modulo (/dev/ttyUSB1) ==" in text


def test_bench_and_replay_prefixes_are_ignored() -> None:
    lines = [f"#N2@901 {line}" for line in module_lines(2)] + ["# t=601 node=2 ligado"]
    assert run("modulo", lines, node=2).verdict == "PASS"
    assert run("probe", [f"@301 {line}" for line in probe_lines()]).verdict == "PASS"


def test_terminal_escape_sequences_are_ignored() -> None:
    lines = [f"\x1b[0;32m{line}\x1b[0m\r" for line in probe_lines()]
    assert run("probe", lines).verdict == "PASS"


# --------------------------------------------------------------------------- defeitos conhecidos


def test_module_flashed_without_the_link_key_fails_with_the_provisioning_hint() -> None:
    lines = [line.replace("LINK_KEY=PROVISIONED", "LINK_KEY=MISSING") for line in module_lines(2)]
    report = run("modulo", lines, node=2)
    assert failed(report) == {"chave-ausente"}          # um defeito, uma falha: a linha esperada que faltou é explicada
    assert "provision_sensor_security.sh --add-link-key" in hint_of(report, "chave-ausente")
    assert "CH-056" in hint_of(report, "chave-ausente")


def test_module_flashed_with_the_probe_number_or_an_old_firmware_is_recognised() -> None:
    probe_as_module = run("modulo", probe_lines(), node=2)
    assert {"partida", "versao-na-partida", "papel", "chave-do-enlace"} <= failed(probe_as_module)
    old = [line for line in module_lines(2) if "[BOOT]" not in line and "NODE_VERSION" not in line]
    report = run("modulo", old, node=2)
    assert {"versao-na-partida", "versao-por-comando", "versao"} <= failed(report)
    assert "Codigo/scripts" in hint_of(report, "versao")


def test_probe_with_a_dead_can_controller_explains_the_missing_line_once() -> None:
    lines = [line for line in probe_lines() if "[GW]" not in line and "FEATURES" not in line]
    lines.insert(4, "[NODE 0] [ERRO] Falha ao iniciar CAN: 0x1")
    report = run("probe", lines)
    assert "controlador-can" in failed(report) and "can-iniciado" not in failed(report)
    assert "MCP2515" in hint_of(report, "controlador-can")


@pytest.mark.parametrize(
    "extra",
    [
        "rst:0xf (BROWNOUT_RST),boot:0x13 (SPI_FAST_FLASH_BOOT)",
        "E (305) Brownout detector was triggered",
        "[NODE 0] [ENERGIA] ATENCAO: reinicio=QUEDA_DE_TENSAO quedas_de_tensao=1 watchdog=0 panico=0 wifi_tx_max_dbm=11.00",
    ],
)
def test_a_brownout_reset_fails_the_probe(extra) -> None:
    report = run("probe", [extra] + probe_lines())
    assert "queda-de-tensao" in failed(report) and "cabo USB" in hint_of(report, "queda-de-tensao")


def test_a_crash_fails_the_board() -> None:
    report = run("modulo", module_lines(2) + ["Guru Meditation Error: Core 1 panic'ed (LoadProhibited)."], node=2)
    assert "falha-do-programa" in failed(report)


def test_probe_missing_a_feature_fails_and_names_it() -> None:
    lines = [line.replace(",RESEARCH", "") for line in probe_lines()]
    report = run("probe", lines)
    assert "recursos" in failed(report)
    detail = next(item for item in report.results if item.name == "recursos")
    assert "RESEARCH" in detail.hint


@pytest.mark.parametrize(
    "old, new, rule",
    [
        ("MPU=YES", "MPU=NO", "sem-mpu"),
        ("DTC=0x0000", "DTC=0x1001", "falha-registrada"),
        ("LINK_KEYS=MASTER", "LINK_KEYS=NONE", "sem-chave"),
        ("LINK_KEYS=MASTER", "LINK_KEYS=UUID_MISMATCH", "chave-de-outro-sensor"),
        ("PROVISIONED=NO BLE_BEACON=ON", "PROVISIONED=NO BLE_BEACON=OFF", "sem-anuncio"),
        ("AUTH=LOCKED", "AUTH=UNLOCKED", "console-destravado"),
    ],
)
def test_vibration_sensor_defects(old, new, rule) -> None:
    lines = [line.replace(old, new) for line in vibration_lines()]
    report = run("vibracao", lines)
    assert failed(report) == {rule}, failed(report)


def test_vibration_sensor_with_the_acoustic_firmware_is_recognised() -> None:
    report = run("vibracao", acoustic_lines())
    assert "firmware-acustico" in failed(report) and "--audio" in hint_of(report, "firmware-acustico")


@pytest.mark.parametrize(
    "mic, rule, words",
    [("NO_HARDWARE", "microfone-ausente", "SD"), ("SILENT", "microfone-mudo", "L/R"), ("STUCK_HIGH", "microfone-preso", "curto")],
)
def test_acoustic_sensor_microphone_states_have_their_own_hint(mic, rule, words) -> None:
    report = run("acustico", acoustic_lines(mic=mic))
    assert failed(report) == {rule}               # o "MIC=OK que faltou" não vira uma segunda falha
    assert words in hint_of(report, rule)


def test_acoustic_sensor_losing_samples_fails() -> None:
    report = run("acustico", acoustic_lines(coverage=87))
    assert failed(report) == {"cobertura"}


def test_acoustic_sensor_without_boot_line_still_passes() -> None:
    """A porta USB abre depois de o sensor ligar: a linha BOOT é opcional."""

    lines = [line for line in acoustic_lines() if not line.startswith("BOOT")]
    assert run("acustico", lines).verdict == "PASS"


def test_missing_command_replies_fail_each_check_once() -> None:
    report = run("vibracao", ["BOOT FIRMWARE=%s COMMIT=%s PROTOCOL=6 PROFILE=VIBRATION" % (VERSION, COMMIT)])
    assert {"versao-por-comando", "mpu", "chaves-do-enlace", "anuncio-ble", "console-travado", "sem-falhas"} <= failed(report)


# --------------------------------------------------------------------------- versão e commit


def test_a_board_with_an_older_version_than_the_repository_fails() -> None:
    report = run("modulo", module_lines(2, version="0.21.1"), node=2)
    assert failed(report) == {"versao-esperada"}
    assert f"o arquivo VERSION diz {VERSION}" in next(i.detail for i in report.results if i.name == "versao-esperada")


def test_versions_mixed_in_one_capture_fail() -> None:
    report = run("modulo", module_lines(2) + ["NODE_VERSION NODE=2 FIRMWARE=0.21.1 COMMIT=%s PROTOCOL=CAN_CLASSIC_V1 FEATURES=X" % COMMIT], node=2)
    assert "versao-unica" in failed(report)
    report = run("modulo", module_lines(2) + ["NODE_VERSION NODE=2 FIRMWARE=%s COMMIT=deadbeef PROTOCOL=CAN_CLASSIC_V1 FEATURES=X" % VERSION], node=2)
    assert "commit-unico" in failed(report)


def test_a_firmware_built_without_the_version_fails() -> None:
    report = run("probe", probe_lines(version="0.0.0-dev", commit="unknown"), expected_version=None)
    assert "versao-definida" in failed(report) and "scripts de Codigo/scripts" in hint_of(report, "versao-definida")


def test_unknown_and_dirty_commits_only_warn_and_strict_turns_them_into_failures(tmp_path) -> None:
    assert warned(run("probe", probe_lines(commit="unknown"))) == {"commit"}
    dirty = run("probe", probe_lines(commit=COMMIT + "-dirty"))
    assert warned(dirty) == {"commit"} and dirty.verdict == "WARN"

    capture = tmp_path / "probe.log"
    capture.write_text("\n".join(probe_lines(commit=COMMIT + "-dirty")), encoding="utf-8")
    assert smoke.main(["probe", "--arquivo", str(capture)]) == 0
    assert smoke.main(["probe", "--arquivo", str(capture), "--estrito"]) == 1


def test_expected_commit_is_compared_by_prefix() -> None:
    assert run("probe", probe_lines(), expected_commit=COMMIT[:6]).verdict == "PASS"
    assert run("probe", probe_lines(commit=COMMIT + "-dirty"), expected_commit=COMMIT).verdict == "WARN"
    report = run("probe", probe_lines(), expected_commit="ffffffff")
    assert failed(report) == {"commit-esperado"}


def test_the_repository_version_file_is_the_default_expectation() -> None:
    assert smoke.read_expected_version() == VERSION
    assert smoke.read_expected_version(Path("/nonexistent")) is None


def test_panel_and_reader_report_version_and_commit() -> None:
    assert run("painel", panel_lines(version="0.21.0")).verdict == "FAIL"
    assert run("leitor", reader_lines(version="0.21.0")).verdict == "FAIL"
    assert run("painel", panel_lines()).commit == COMMIT


# --------------------------------------------------------------------------- autoteste (SELFTEST)


def selftest_lines(node, verdict="PASS", extra=()):
    return [
        f"[NODE {node}] [SELFTEST] INICIO PARTES=CAN ESTADO=IDLE",
        f"[NODE {node}] [SELFTEST] CHECK=CAN_LOOPBACK RESULT={verdict} REASON=OK FRAMES=4/4",
        *extra,
        f"[NODE {node}] [SELFTEST] RESULT={verdict} CHECKS=2 WARN=0 FAIL=0 DURATION_MS=6",
    ]


def test_selftest_pass_fail_warn_and_refused() -> None:
    assert run("modulo", module_lines(2) + selftest_lines(2), node=2, selftest=True).verdict == "PASS"
    assert run("probe", probe_lines() + selftest_lines(0), selftest=True).verdict == "PASS"

    refused = run("modulo", module_lines(2) + ["[NODE 2] [SELFTEST] RECUSADO ESTADO=FOLLOWER: o modulo participa da rede"], node=2, selftest=True)
    assert failed(refused) == {"autoteste-recusado"} and "antes de ligar o barramento" in hint_of(refused, "autoteste-recusado")

    bad = run("modulo", module_lines(2) + selftest_lines(2, "FAIL"), node=2, selftest=True)
    assert failed(bad) == {"autoteste-falhou"}
    warn = run("modulo", module_lines(2) + selftest_lines(2, "WARN"), node=2, selftest=True)
    assert warned(warn) == {"autoteste-aviso"} and warn.verdict == "WARN"

    missing = run("modulo", module_lines(2), node=2, selftest=True)
    assert failed(missing) == {"autoteste"}
    # Sem --selftest o autoteste nem é pedido.
    assert "autoteste" not in {item.name for item in run("modulo", module_lines(2), node=2).results}


def test_selftest_is_only_for_modules_and_the_probe() -> None:
    with pytest.raises(ValueError):
        smoke.build_plan("vibracao", None, selftest=True)
    with pytest.raises(ValueError):
        smoke.build_plan("modulo", None)
    with pytest.raises(ValueError):
        smoke.build_plan("modulo", 32)


# --------------------------------------------------------------------------- a captura pela porta serial


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class FakeBoard:
    """Uma placa de mentira: escreve a partida quando é reiniciada e responde aos comandos."""

    def __init__(self, clock, boot=(), replies=None, on_open=()):
        self.clock = clock
        self.queue = list(on_open)
        self.boot = list(boot)
        self.replies = replies or {}
        self.resets = 0
        self.sent = []

    def readline(self):
        self.clock.now += 0.2           # cada leitura sem dado gasta o tempo limite de 0,2 s da porta
        return (self.queue.pop(0) + "\n").encode() if self.queue else b""

    def write(self, data):
        command = data.decode().strip()
        self.sent.append(command)
        self.queue.extend(self.replies.get(command, []))

    def pulse_reset(self):
        self.resets += 1
        self.queue = list(self.boot)


def test_esp32_roles_are_reset_then_read_until_the_boot_ends_and_queried() -> None:
    clock = FakeClock()
    boot = module_lines(2)[:-1]
    board = FakeBoard(clock, boot=boot, replies={"VERSION": [module_lines(2)[-1]]})
    plan = smoke.build_plan("modulo", 2)
    lines = smoke.collect(plan, board, clock=clock)
    assert board.resets == 1 and board.sent == ["VERSION"]
    assert smoke.evaluate(plan, lines, expected_version=VERSION).verdict == "PASS"
    assert clock.now < 3        # parou quando a partida terminou, sem esperar os 12 s


def test_a_silent_esp32_waits_for_the_whole_timeout() -> None:
    clock = FakeClock()
    board = FakeBoard(clock)
    plan = smoke.build_plan("probe")
    lines = smoke.collect(plan, board, boot_seconds=5, command_seconds=1, clock=clock)
    assert lines == [] and board.resets == 1 and board.sent == ["PROBE_VERSION", "PROBE_STATUS"]
    assert clock.now >= 5 + 2 - 0.01


def test_pico_sensors_are_not_reset_and_answer_three_commands() -> None:
    clock = FakeClock()
    status, net = vibration_lines()[2], vibration_lines()[3]
    board = FakeBoard(clock, replies={"VERSION": [vibration_lines()[1]], "STATUS": [status], "NET": [net]})
    plan = smoke.build_plan("vibracao")
    lines = smoke.collect(plan, board, clock=clock)
    assert board.resets == 0 and board.sent == ["VERSION", "STATUS", "NET"]
    assert smoke.evaluate(plan, lines, expected_version=VERSION).verdict == "PASS"
    # --reset força o reinício, --sem-reset o impede.
    forced = FakeBoard(clock)
    smoke.collect(plan, forced, reset=True, clock=clock)
    assert forced.resets == 1
    skipped = FakeBoard(clock)
    smoke.collect(smoke.build_plan("probe"), skipped, reset=False, boot_seconds=1, command_seconds=1, clock=clock)
    assert skipped.resets == 0


def test_the_acoustic_sensor_waits_for_the_microphone_to_measure_a_window() -> None:
    clock = FakeClock()
    board = FakeBoard(clock)
    smoke.collect(smoke.build_plan("acustico"), board, command_seconds=0.2, clock=clock)
    assert clock.now >= 2.0 + 3 * 0.2 - 0.01


# --------------------------------------------------------------------------- linha de comando


def test_cli_reads_a_capture_and_sets_the_exit_code(tmp_path, capsys) -> None:
    good = tmp_path / "bom.log"
    good.write_text("\n".join(module_lines(3)), encoding="utf-8")
    assert smoke.main(["modulo", "--no", "3", "--arquivo", str(good)]) == 0
    assert "SMOKE RESULT=PASS ROLE=modulo" in capsys.readouterr().out

    bad = tmp_path / "ruim.log"
    bad.write_text("\n".join(module_lines(3)).replace("PROVISIONED", "MISSING"), encoding="utf-8")
    assert smoke.main(["modulo", "--no", "3", "--arquivo", str(bad)]) == 1
    out = capsys.readouterr().out
    assert "FALHA  chave-ausente" in out and "-> a placa foi gravada sem a chave do enlace" in out
    assert "SMOKE RESULT=FAIL ROLE=modulo" in out


def test_cli_exit_codes_for_misuse_and_for_a_silent_board(tmp_path, capsys) -> None:
    empty = tmp_path / "vazio.log"
    empty.write_text("\n\n", encoding="utf-8")
    assert smoke.main(["probe", "--arquivo", str(empty)]) == 3
    captured = capsys.readouterr()
    assert "SMOKE RESULT=FAIL" in captured.out and "nenhuma linha recebida" in captured.err
    with pytest.raises(SystemExit) as error:
        smoke.main(["modulo", "--arquivo", str(empty)])          # falta --no
    assert error.value.code == 2
    with pytest.raises(SystemExit) as error:
        smoke.main(["probe"])                                    # falta a origem
    assert error.value.code == 2


def test_cli_version_options(tmp_path, capsys) -> None:
    capture = tmp_path / "probe.log"
    capture.write_text("\n".join(probe_lines(version="0.21.0")), encoding="utf-8")
    assert smoke.main(["probe", "--arquivo", str(capture)]) == 1                           # VERSION do repositório
    assert smoke.main(["probe", "--arquivo", str(capture), "--versao-esperada", "0.21.0"]) == 0
    assert smoke.main(["probe", "--arquivo", str(capture), "--versao-esperada", "nenhuma"]) == 0
    capsys.readouterr()


def test_cli_opens_the_port_and_always_closes_it(monkeypatch, capsys) -> None:
    clock = FakeClock()
    ports: list = []

    class Port(FakeBoard):
        def __init__(self, path, baud):
            super().__init__(clock, boot=probe_lines()[:-2],
                             replies={"PROBE_VERSION": [probe_lines()[-2]], "PROBE_STATUS": [probe_lines()[-1]]})
            self.opened = (path, baud)
            self.closed = False
            ports.append(self)

        def close(self):
            self.closed = True

    monkeypatch.setattr(smoke, "SerialPort", Port)
    assert smoke.main(["probe", "--porta", "/dev/ttyUSB7", "--baud", "230400"], clock=clock) == 0
    assert ports[0].opened == ("/dev/ttyUSB7", 230400) and ports[0].closed
    assert "(/dev/ttyUSB7)" in capsys.readouterr().out


# --------------------------------------------------------------------------- sobre o que o firmware real escreve


def test_probe_boot_in_the_committed_replays_passes() -> None:
    """A Probe da bancada virtual, o firmware real, escreve o que o script espera na partida."""

    plan = smoke.build_plan("probe")
    boot_only = dataclasses.replace(
        plan, checks=tuple(c for c in plan.checks if c.name not in ("versao-por-comando", "estado"))
    )
    for name in ("formation", "sensor", "owner_failover"):
        lines = [smoke.normalize(line) for line in (REPLAYS / f"{name}.log").read_text(encoding="utf-8").splitlines()]
        report = smoke.evaluate(boot_only, lines, expected_version=VERSION)
        assert report.verdict == "PASS", (name, [(i.name, i.detail) for i in report.results if i.level != smoke.OK])
        assert report.commit == "sim"


@pytest.fixture(scope="module")
def bench_output(tmp_path_factory) -> str:
    """Saída serial de todos os equipamentos do cenário 'selftest' da bancada virtual (firmware real)."""

    if shutil.which("cc") is None or shutil.which("c++") is None:
        pytest.skip("compilador C/C++ indisponível")
    binary = tmp_path_factory.mktemp("bench") / "sim_network"
    subprocess.run([str(ROOT / "Codigo/scripts/build_network_sim.sh"), str(binary)], check=True, capture_output=True)
    done = subprocess.run([str(binary), "selftest", "--verbose"], check=True, capture_output=True, text=True)
    return done.stdout


def bench_lines(output: str, node: int) -> list[str]:
    prefix = re.compile(rf"^#N{node}@\d+ " if node else r"^@\d+ ")
    return [smoke.normalize(line) for line in output.splitlines() if prefix.match(line)]


@needs_cxx
def test_real_firmware_output_passes_for_the_probe_and_a_healthy_module(bench_output) -> None:
    probe = smoke.evaluate(smoke.build_plan("probe", None, selftest=True), bench_lines(bench_output, 0),
                           expected_version=VERSION)
    assert probe.verdict == "PASS", [(i.name, i.detail) for i in probe.results if i.level != smoke.OK]

    # O Node 1 passa no autoteste; depois, já dentro da rede, recusa (o que o cenário também confere).
    lines = bench_lines(bench_output, 1)
    first_refusal = next(index for index, line in enumerate(lines) if "RECUSADO" in line)
    module = smoke.evaluate(smoke.build_plan("modulo", 1, selftest=True), lines[:first_refusal], expected_version=VERSION)
    assert module.verdict == "PASS", [(i.name, i.detail) for i in module.results if i.level != smoke.OK]
    refused = smoke.evaluate(smoke.build_plan("modulo", 1, selftest=True), lines, expected_version=VERSION)
    assert "autoteste-recusado" in failed(refused)


@needs_cxx
def test_real_firmware_output_fails_the_module_with_a_broken_controller(bench_output) -> None:
    report = smoke.evaluate(smoke.build_plan("modulo", 2, selftest=True), bench_lines(bench_output, 2),
                            expected_version=VERSION)
    assert "autoteste-falhou" in failed(report)
    assert not (failed(report) - {"autoteste-falhou"}), failed(report)     # só o controlador: o resto da placa está certo
