"""Métricas da rede, reassociação automática e ensaios de falha (v0.18).

Níveis de verificação:

1. contrato de código-fonte do firmware do Node CAN;
2. conferência cruzada da biblioteca C (duração de quadro, PDU autenticada)
   com implementações de referência em Python;
3. análise de tempo de resposta contra o exemplo publicado por Davis et al.;
4. ponta a ponta: as gravações em ``Front/replays`` são a saída da Probe 00
   na bancada virtual (firmware real do Node CAN em vários Nodes simulados).
   Elas alimentam o decodificador, o domínio, as métricas e o registrador de
   ensaios da TUI, e são conferidas com uma nova execução da bancada.

Os níveis 2 e a conferência das gravações precisam de compilador C/C++; sem
ele são ignorados (a integração contínua sempre os executa).
"""
from __future__ import annotations

import asyncio
import hashlib
import hmac
import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from pico_tui import commands
from pico_tui.app import PicoTuiApp
from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    BusStatsReceived,
    CommandAck,
    FaultCommandSent,
    FaultEventReceived,
    IdStatsReceived,
    NetworkEventReceived,
    WirelessAssociationReceived,
    WirelessFailoverReceived,
    WirelessLinkReceived,
)
from pico_tui.core.state_store import StateStore
from pico_tui.dialogs import ChoiceScreen, ConfirmScreen, ParameterScreen
from pico_tui.metrics import rta
from pico_tui.metrics.catalog import describe
from pico_tui.metrics.experiments import EXPERIMENTS, MEASURE_LABELS, ExperimentRecorder
from pico_tui.metrics.export import export_metrics, snapshot_to_dict
from pico_tui.metrics.service import MetricsService
from pico_tui.metrics_screens import ExperimentsScreen, MetricsScreen
from pico_tui.protocol.router import DecoderRouter
from pico_tui.security import SecurityManager
from pico_tui.services.controller import DomainController
from pico_tui.widgets import WrappedLines
from tests.conftest import settle

ROOT = Path(__file__).resolve().parents[2]
CODE = ROOT / "Codigo"
LINK = CODE / "common" / "ioc_link"
NODE = CODE / "node-can"
REPLAYS = ROOT / "Front" / "replays"
# Cenários da bancada virtual (Codigo/node-can/test/host/sim_network.cpp).
SCENARIOS = (
    "formation", "sensor", "owner_failover", "sensor_moves", "manual_policy", "leader_failure",
    "leader_failure_no_probe", "silence_leader", "ap_outage", "drop_session", "bus_load", "two_sensors",
    "failover_retry", "failover_dead_target", "silence_leader_with_sensor",
)
UUID = "0xE6616408432B6F39"

needs_cc = pytest.mark.skipif(shutil.which("cc") is None, reason="compilador C indisponível")
needs_cxx = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("c++") is None, reason="compilador C/C++ indisponível"
)


# --------------------------------------------------------------------------
# 1. Contrato do firmware
# --------------------------------------------------------------------------


def test_can_identifiers_for_experiments_do_not_collide_with_project_traffic() -> None:
    ids = (NODE / "include/can_ids.h").read_text()
    assert "#define CAN_ID_TEST_CMD                   0x305" in ids
    assert "#define CAN_ID_TEST_STATUS                0x306" in ids
    assert "#define CAN_ID_TEST_LOAD_HIGH             0x010" in ids
    assert "#define CAN_ID_TEST_LOAD_LOW              0x7EF" in ids
    # A carga de alta prioridade vence a eleição (0x050); a de baixa perde
    # para os dados de sensores (até 0x39F).
    numbers = {name: int(value, 16) for name, value in re.findall(r"#define (CAN_ID_\w+)\s+(0x[0-9A-Fa-f]+)", ids)}
    assert numbers["CAN_ID_TEST_LOAD_HIGH"] < min(v for k, v in numbers.items() if "TEST_LOAD" not in k)
    assert numbers["CAN_ID_TEST_LOAD_LOW"] > max(v for k, v in numbers.items() if "TEST_LOAD" not in k)
    for can_id, expected in ((0x100, "presença"), (0x381, "Módulo 01"), (0x305, "ensaio"), (0x322, "Módulo 01")):
        assert expected in describe(can_id).name


def test_probe_measures_bus_and_nodes_stay_silent_on_metrics() -> None:
    source = (NODE / "src/net_metrics.cpp").read_text()
    assert "ioc_can_frame_bits" in source
    assert "[GW] BUS_STATS" in source and "[GW] ID_STATS" in source
    # Todas as funções retornam de imediato nos Nodes funcionais.
    for name in ("netMetricsOnReceive", "netMetricsOnTransmit", "netMetricsOnTransfer", "netMetricsPoll"):
        body = source.split(f"void {name}(")[1].split("}")[0]
        assert "NODE_ID != 0" in body, name
    main = (NODE / "src/main.ino").read_text()
    assert "netMetricsOnReceive(rx);" in main and "netMetricsPoll();" in main


def test_fault_injection_can_be_compiled_out_and_is_bounded() -> None:
    header = (NODE / "include/fault_injection.h").read_text()
    source = (NODE / "src/fault_injection.cpp").read_text()
    proto = (NODE / "include/protocolo.h").read_text()
    assert "#ifndef IOT_FAULT_INJECTION" in header and "#define IOT_FAULT_INJECTION 1" in header
    assert "#if IOT_FAULT_INJECTION" in source
    assert "#define TEST_MAX_DURATION_MS            120000UL" in proto
    assert "TEST_MAGIC_CMD" in source and "clampDuration" in source
    # O Node em silêncio não confirma quadros (modo somente escuta) e só ouve o cancelamento.
    assert "ACAN2515Settings::ListenOnlyMode" in source
    main = (NODE / "src/main.ino").read_text()
    assert "if (faultInjectionSilent())" in main
    assert "discarded.id == CAN_ID_TEST_CMD" in main


def test_failover_policy_is_opt_in_and_decided_by_the_leader() -> None:
    source = (NODE / "src/wireless_discovery.cpp").read_text()
    proto = (NODE / "include/protocolo.h").read_text()
    assert "WIRELESS_ASSOC_ACTION_BIND_AUTO     0x03" in proto
    assert "WIRELESS_ASSOC_ACTION_FAILOVER_OWNER_LOST  0x11" in proto
    assert "bool autoFailover = false;" in source           # política padrão: manual
    assert "if (state != STATE_LEADER) return;" in source   # só o líder decide
    assert "ioc_fo_poll(&failoverTable" in source
    assert "ioc_fo_should_yield(" in source
    # Uma sessão de dados ativa conta como presença do sensor.
    assert "if (secure) binding.lastSeenMs = now;" in source
    assert "[GW] WIRELESS_FAILOVER uuid=" in source
    library = (LINK / "ioc_failover.c").read_text()
    assert "if (!entry->auto_failover) continue;" in library


def test_followers_watch_the_leader_without_the_probe() -> None:
    main = (NODE / "src/main.ino").read_text()
    assert "#define IOT_FOLLOWER_LEADER_WATCHDOG 1" in main
    assert "#define FOLLOWER_LEADER_TIMEOUT_MULTIPLIER 6" in main
    assert "#define GATEWAY_LEADER_TIMEOUT_MULTIPLIER  4" in main   # a Probe continua reagindo antes
    assert "void followerCheckLeaderFailure()" in main
    assert "void rearmWatchdogsAfterStall(" in main
    # Dois líderes por um instante: o seguidor adota o de maior identificador.
    assert "senderLeader != leaderId && senderLeader > leaderId" in main


def test_can_fd_predisposition_is_present_but_not_wired() -> None:
    assert "ioc_wd_build_segment_ex" in (LINK / "ioc_wdata.h").read_text()
    assert "IOC_SECPDU_MAC_FD" in (LINK / "ioc_secpdu.h").read_text()
    for source in (NODE / "src").iterdir():
        text = source.read_text()
        assert "ioc_secpdu" not in text, source.name
        assert "_ex(" not in text, source.name
    assert (ROOT / "Documentacao/arquitetura/predisposicao-can-fd.md").exists()


# --------------------------------------------------------------------------
# 2. Biblioteca C x referência em Python
# --------------------------------------------------------------------------


def _can_stream(can_id: int, data: bytes) -> list[int]:
    bits = [0]                                             # SOF
    bits += [(can_id >> i) & 1 for i in range(10, -1, -1)]
    bits += [0, 0, 0]                                      # RTR, IDE, r0
    bits += [(len(data) >> i) & 1 for i in range(3, -1, -1)]
    for byte in data:
        bits += [(byte >> i) & 1 for i in range(7, -1, -1)]
    return bits


def _crc15(bits: list[int]) -> int:
    crc = 0
    for bit in bits:
        top = ((crc >> 14) & 1) ^ bit
        crc = (crc << 1) & 0x7FFF
        if top:
            crc ^= 0x4599
    return crc


def _frame_bits(can_id: int, data: bytes) -> int:
    bits = _can_stream(can_id, data)
    crc = _crc15(bits)
    bits += [(crc >> i) & 1 for i in range(14, -1, -1)]
    stuffed, run, last = 0, 0, None
    for bit in bits:
        if bit == last:
            run += 1
        else:
            run, last = 1, bit
        if run == 5:
            stuffed += 1
            last, run = 1 - bit, 1                         # o bit inserido conta para a sequência seguinte
    return len(bits) + stuffed + 13


@needs_cc
def test_c_bus_measurement_matches_python_reference(tmp_path: Path) -> None:
    binary = tmp_path / "test_ioc_net"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{LINK}",
         *(str(LINK / name) for name in ("ioc_sha256.c", "ioc_wdata.c", "ioc_canbits.c", "ioc_secpdu.c", "ioc_failover.c")),
         str(CODE / "common/tests/test_ioc_net.c"), "-lm", "-o", str(binary)],
        check=True,
    )
    assert "PASS" in subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout
    vectors = dict(
        line.split("=", 1)
        for line in subprocess.run([str(binary), "--vectors"], check=True, capture_output=True, text=True).stdout.split()
    )
    data = bytes.fromhex("0123456789abcdef")
    assert int(vectors["crc15_0x123_8"], 16) == _crc15(_can_stream(0x123, data))
    assert int(vectors["bits_0x123_8"]) == _frame_bits(0x123, data)
    assert int(vectors["crc15_0x100_5"], 16) == _crc15(_can_stream(0x100, data[:5]))
    assert int(vectors["bits_0x100_5"]) == _frame_bits(0x100, data[:5])
    assert int(vectors["bits_0x000_0"]) == _frame_bits(0x000, b"")
    assert int(vectors["bits_0x7ff_8_ff"]) == _frame_bits(0x7FF, b"\xff" * 8)
    # O pior caso usado na análise de tempo de resposta é o mesmo do firmware.
    for dlc in range(9):
        assert int(vectors[f"worst_{dlc}"]) == rta.frame_bits_worst(dlc)
        for can_id, payload in ((0x000, b"\x00" * dlc), (0x7FF, b"\xff" * dlc), (0x555, b"\x0f" * dlc)):
            assert _frame_bits(can_id, payload) <= rta.frame_bits_worst(dlc)

    # PDU autenticada (predisposição para CAN FD): carga | atualidade(1) | MAC(8).
    key = bytes(range(32))
    freshness = 0x0102030405 + 1
    mac = hmac.new(key, (0x304).to_bytes(2, "big") + data + freshness.to_bytes(8, "big"), hashlib.sha256).digest()
    assert vectors["secpdu"] == (data + bytes([freshness & 0xFF]) + mac[:8]).hex()


# --------------------------------------------------------------------------
# 3. Análise de tempo de resposta
# --------------------------------------------------------------------------


def test_response_time_analysis_reproduces_the_published_counterexample() -> None:
    """Davis, Burns, Bril e Lukkien (2007), seção 3.

    Três mensagens com 1 ms de transmissão e períodos de 2,5, 3,5 e 3,5 ms.
    A análise original (Tindell, 1995) dá 3 ms para a de menor prioridade; o
    valor correto, alcançado pela segunda instância do período ocupado, é
    3,5 ms. A 135 kbit/s um quadro de 8 bytes, no pior caso, dura 1 ms.
    """

    assert rta.frame_bits_worst(8) == 135
    flows = [rta.Flow(1, "A", 8, 2500.0), rta.Flow(2, "B", 8, 3500.0), rta.Flow(3, "C", 8, 3500.0)]
    analysis = rta.analyze(flows, bitrate=135_000)
    assert [round(result.frame_us) for result in analysis.results] == [1000, 1000, 1000]
    assert [result.response_us for result in analysis.results] == [2000.0, 3000.0, 3500.0]
    assert analysis.results[2].response_first_us == 3000.0     # o que a análise original responderia
    assert analysis.schedulable and analysis.critical.flow.name == "C"
    assert abs(analysis.utilization - (1 / 2.5 + 2 / 3.5)) < 1e-9


def test_response_time_analysis_handles_overload_bursts_and_traffic_without_deadline() -> None:
    overloaded = rta.analyze([rta.Flow(1, "A", 8, 500.0), rta.Flow(2, "B", 8, 500.0)], bitrate=135_000)
    assert not overloaded.schedulable
    assert overloaded.results[1].response_us is None and overloaded.results[1].slack_percent == -100.0

    # Rajada: 9 quadros por segundo no mesmo identificador.
    burst = rta.Flow(0x381, "telemetria", 8, 1_000_000.0, burst=9)
    alone = rta.analyze([burst]).results[0]
    assert alone.response_first_us == pytest.approx(270.0)
    assert alone.response_us == pytest.approx(9 * 270.0)
    with_heartbeat = rta.analyze([rta.Flow(0x100, "presença", 5, 1_000_000.0), burst]).results[1]
    assert with_heartbeat.response_us == pytest.approx(9 * 270.0 + 210.0)

    # Carga artificial: interfere, mas não tem prazo próprio.
    load = rta.Flow(0x010, "carga", 8, 600.0, has_deadline=False)
    loaded = rta.analyze([load, rta.Flow(0x100, "presença", 5, 1_000_000.0)])
    assert loaded.schedulable and loaded.results[0].slack_percent is None
    # Um quadro de carga à frente do sinal de presença: 270 us de espera mais 210 us de transmissão.
    assert loaded.results[1].response_us == pytest.approx(270.0 + 210.0)
    assert loaded.critical.flow.can_id == 0x100


def test_flows_are_estimated_from_probe_statistics() -> None:
    periodic = rta.flow_from_observation(
        0x100, "presença", count=5, window_ms=5000, dlc=5, dt_min_us=999_600, dt_avg_us=1_000_000, dt_max_us=1_000_400
    )
    # Período = menor intervalo entre chegadas; sem variação de liberação.
    assert periodic.burst == 1 and periodic.period_us == 999_600 and periodic.jitter_us == 0 and not periodic.estimated

    # Um ciclo perdido dobra dt_max e não muda o modelo.
    skipped = rta.flow_from_observation(
        0x100, "presença", count=4, window_ms=5000, dlc=5, dt_min_us=999_600, dt_avg_us=1_250_000, dt_max_us=2_000_100
    )
    assert skipped.burst == 1 and skipped.period_us == 999_600 and skipped.jitter_us == 0

    # Três respostas a um único comando, em 4 ms: um episódio na janela, não um período de 2 ms.
    episode = rta.flow_from_observation(
        0x200, "estado", count=3, window_ms=5000, dlc=8, dt_min_us=1_800, dt_avg_us=2_000, dt_max_us=2_200
    )
    assert episode.burst == 3 and episode.period_us == 5_000_000 and episode.estimated

    # Fluxo denso (carga artificial): o intervalo mínimo é efeito da fila; vale o médio.
    dense = rta.flow_from_observation(
        0x010, "carga", count=9000, window_ms=5000, dlc=8, dt_min_us=0, dt_avg_us=555, dt_max_us=1_400
    )
    assert dense.burst == 1 and dense.period_us == 555

    # Nove quadros por segundo: 8 intervalos curtos e um longo.
    telemetry = rta.flow_from_observation(
        0x381, "dados", count=45, window_ms=5000, dlc=8, dt_min_us=0, dt_avg_us=111_111, dt_max_us=996_000
    )
    assert telemetry.burst == 9 and telemetry.period_us == pytest.approx(1_000_000, rel=0.01) and telemetry.estimated

    sporadic = rta.flow_from_observation(
        0x304, "comando", count=1, window_ms=5000, dlc=8, dt_min_us=0, dt_avg_us=0, dt_max_us=0
    )
    assert sporadic.burst == 1 and sporadic.period_us == 5_000_000
    assert rta.flow_from_observation(0x304, "x", count=0, window_ms=5000, dlc=8, dt_min_us=0, dt_avg_us=0, dt_max_us=0) is None


def test_capacity_estimate_is_bounded_by_deadlines_and_by_the_utilization_ceiling() -> None:
    base = [rta.Flow(0x100, "presença", 5, 1_000_000.0), rta.Flow(0x200, "local", 5, 1_000_000.0)]
    theoretical = rta.capacity(base)
    guideline = rta.capacity(base, utilization_limit=0.40)
    # Cada sensor acrescenta 9 quadros de 270 us por segundo: 0,243 % do barramento.
    assert 400 <= theoretical.extra_sensors <= 412 and not theoretical.limit_reached
    assert theoretical.utilization <= 1.0
    assert 160 <= guideline.extra_sensors <= 165 and guideline.utilization <= 0.40
    slow = rta.capacity(base, bitrate=125_000)
    assert slow.extra_sensors < theoretical.extra_sensors / 3.5
    assert rta.capacity([]).extra_sensors == 0
    saturated = rta.capacity([rta.Flow(1, "A", 8, 250.0)])
    assert saturated.extra_sensors == 0


# --------------------------------------------------------------------------
# 4. Decodificação das novas linhas
# --------------------------------------------------------------------------


def _new_domain(clock=None):
    bus = EventBus()
    state = StateStore()
    DomainController(bus, state)
    router = DecoderRouter(bus, requested_mode="gateway")
    events: dict[type, list] = {}
    for event_type in (
        BusStatsReceived, IdStatsReceived, FaultCommandSent, FaultEventReceived, NetworkEventReceived,
        WirelessFailoverReceived, WirelessAssociationReceived, WirelessLinkReceived, CommandAck,
    ):
        events[event_type] = []
        bus.subscribe(event_type, events[event_type].append)
    kwargs = {"clock": clock} if clock else {}
    metrics = MetricsService(bus, **kwargs)
    recorder = ExperimentRecorder(bus, **kwargs)
    return bus, state, router, events, metrics, recorder


def _feed(router: DecoderRouter, *lines: str) -> None:
    async def run() -> None:
        for line in lines:
            await router.decode(line)

    asyncio.run(run())


def test_probe_metric_and_experiment_lines_are_decoded() -> None:
    _bus, state, router, events, metrics, _recorder = _new_domain()
    _feed(
        router,
        "[GW] BUS_STATS t_ms=17300 win_ms=1000 frames=42 bits=4718 load=0.94 peak=2.63 rx_err=3 tx_err=130 "
        "eflg=0x15 rx_peak=7 ids=11 xfer_ok=20 xfer_err=1 own=2 untracked=0",
        "[GW] ID_STATS id=0x100 win_ms=5000 n=5 dlc=5 bits=469 dt_min_us=999500 dt_avg_us=1000020 dt_max_us=1000700",
        "[GW] FAULT_TX kind=POWER_CYCLE node=3 duration_ms=20000 seq=4",
        "[GW] FAULT_EVENT node=3 kind=POWER_CYCLE state=STARTED duration_ms=20000 seq=4",
        "[GW] FAULT_EVENT node=0 kind=BUS_LOAD state=ENDED duration_ms=10000 seq=5 load_pct=40 priority=HIGH "
        "frames=17316 achieved_pct=39.99",
        f"[GW] WIRELESS_FAILOVER uuid={UUID} from=1 to=2 reason=OWNER_LOST rssi=-68",
        "[GW] Falha detectada no lider NODE 3",
        "[GW] Requisicao de eleicao observada",
        "[GW] Lider anunciado: NODE 2",
        "[GW] JOIN observado de NODE 3",
        "[GW] FAULT_ERROR use: FAULT POWER_CYCLE|SILENCE|AP_OUTAGE <node> <segundos>",
    )
    stats = events[BusStatsReceived][0]
    assert (stats.frames, stats.bits, stats.load_percent, stats.peak_percent) == (42, 4718, 0.94, 2.63)
    assert (stats.rx_errors, stats.tx_errors, stats.error_flags, stats.rx_queue_peak) == (3, 130, 0x15, 7)
    assert (stats.transfers_ok, stats.transfers_err, stats.own_frames) == (20, 1, 2)
    ident = events[IdStatsReceived][0]
    assert (ident.can_id, ident.count, ident.dlc, ident.dt_avg_us) == (0x100, 5, 5, 1000020)
    sent = events[FaultCommandSent][0]
    assert (sent.kind, sent.node_id, sent.duration_ms, sent.seq) == ("POWER_CYCLE", 3, 20000, 4)
    started, ended = events[FaultEventReceived]
    assert (started.node_id, started.kind, started.state) == (3, "POWER_CYCLE", "STARTED")
    assert ended.kind == "BUS_LOAD" and ended.payload["ACHIEVED_PCT"] == "39.99"
    failover = events[WirelessFailoverReceived][0]
    assert (failover.from_node_id, failover.to_node_id, failover.reason, failover.rssi_dbm) == (1, 2, "OWNER_LOST", -68)
    assert [event.kind for event in events[NetworkEventReceived]] == [
        "LEADER_FAILURE_DETECTED", "ELECTION_REQUESTED", "LEADER_ANNOUNCED", "JOIN_OBSERVED",
    ]
    # O estado de erro do controlador da Probe chega à tela de rede.
    network = state.snapshot().network
    assert network.utilization_percent == 0.94 and network.error_passive and network.error_warning
    assert not network.bus_off
    assert metrics.snapshot().error_state == "ERROR_PASSIVE"
    assert state.snapshot().nodes[2].role == "LEADER"


def test_association_and_link_lines_remain_compatible_with_older_firmware() -> None:
    _bus, state, router, events, metrics, _recorder = _new_domain()
    old_assoc = f"[GW] WIRELESS_ASSOC node=1 child=1 uuid={UUID} profile=VIBRATION state=ONLINE rssi=-48 protocol=6"
    old_link = (
        "[GW] WIRELESS_LINK node=1 child=1 state=SECURE rssi=-47 age_s=3 rx=10 "
        "auth_fail=0 replay=0 lost=0 stream=ON period_ms=1000"
    )
    _feed(router, old_assoc, old_link)
    assoc, link = events[WirelessAssociationReceived][0], events[WirelessLinkReceived][0]
    assert assoc.failover_auto is None and assoc.session_secure is None
    assert link.rtt_ms is None and link.rtt_max_ms is None
    sensor = state.find_sensor("01.01")
    assert sensor.failover_policy == "UNKNOWN" and sensor.data_link_rtt_ms is None

    _feed(
        router,
        old_assoc + " failover=AUTO session=SECURE",
        old_link + " rtt_ms=3.4 rtt_max_ms=12.1",
        "ACK COMMAND=STATUS STATE=APPLIED TX=B00001 NODE=1 CHILD=1 DETAIL=0 RTT_MS=37",
        "ACK COMMAND=CONFIG STATE=REJECTED TX=B00002 NODE=1 CHILD=1 DETAIL=2 REASON=SENSOR_REJECTED RTT_MS=41",
    )
    assoc, link = events[WirelessAssociationReceived][1], events[WirelessLinkReceived][1]
    assert assoc.failover_auto is True and assoc.session_secure is True
    assert (link.rtt_ms, link.rtt_max_ms) == (3.4, 12.1)
    sensor = state.find_sensor("01.01")
    assert sensor.failover_policy == "AUTO" and sensor.data_link_rtt_ms == 3.4
    assert events[CommandAck][-1].payload["REASON"] == "SENSOR_REJECTED"
    snapshot = metrics.snapshot()
    assert snapshot.commands.count == 1 and snapshot.commands.average == 37.0 and snapshot.commands.rejected == 1
    assert snapshot.links[0].rtt_ms == 3.4 and snapshot.links[0].sessions == 1

    # Sem medida, o firmware informa -1.
    _feed(router, old_link.replace("SECURE", "OFFERING") + " rtt_ms=-1 rtt_max_ms=-1")
    assert events[WirelessLinkReceived][2].rtt_ms is None


def test_sensor_follows_its_new_parent_after_a_failover() -> None:
    _bus, state, router, _events, metrics, _recorder = _new_domain()
    assoc = "[GW] WIRELESS_ASSOC node={node} child=1 uuid=" + UUID + " profile=VIBRATION state={state} rssi=-50 protocol=6 failover=AUTO session=NONE"
    _feed(router, "[GW] No descoberto: NODE 2", assoc.format(node=1, state="ONLINE"))
    assert state.find_sensor("01.01") is not None
    # O módulo antigo saiu do ar: não há UNBOUND. O novo vínculo retira o anterior.
    _feed(
        router,
        f"[GW] WIRELESS_FAILOVER uuid={UUID} from=1 to=2 reason=OWNER_LOST rssi=-68",
        assoc.format(node=2, state="ASSOCIATING"),
    )
    assert state.find_sensor("01.01") is None
    assert state.find_sensor("02.01").failover_policy == "AUTO"
    incidents = [incident.message for incident in state.find_node(2).incidents]
    assert any("Reassociação automática" in message and "saiu do ar" in message for message in incidents)
    assert [link.logical_id for link in metrics.snapshot().links] == ["02.01"]
    # Um relato atrasado do módulo antigo, já sem alcance, não desfaz a troca.
    _feed(router, assoc.format(node=1, state="LOST"))
    assert state.find_sensor("02.01") is not None


def test_commands_for_experiments_and_policy_are_validated_and_protected(tmp_path: Path) -> None:
    assert commands.wireless_association("BIND", 1, UUID) == f"WIRELESS BIND 1 {UUID}"
    assert commands.wireless_association("BIND", 1, UUID, "AUTO") == f"WIRELESS BIND 1 {UUID} AUTO"
    assert commands.wireless_association("POLICY", 2, UUID, "manual") == f"WIRELESS POLICY 2 {UUID} MANUAL"
    assert commands.wireless_association("UNBIND", 2, UUID, "AUTO") == f"WIRELESS UNBIND 2 {UUID}"
    with pytest.raises(ValueError):
        commands.wireless_association("POLICY", 2, UUID)
    with pytest.raises(ValueError):
        commands.wireless_association("BIND", 2, UUID, "MANUAL")
    assert commands.fault("power_cycle", 3, 20) == "FAULT POWER_CYCLE 3 20"
    assert commands.fault("DROP_SESSION", 1) == "FAULT DROP_SESSION 1"
    assert commands.fault_load(40, 10, "low") == "FAULT LOAD 40 10 LOW"
    assert commands.fault_cancel() == "FAULT CANCEL" and commands.fault_cancel(3) == "FAULT CANCEL 3"
    for bad in (lambda: commands.fault("POWER_CYCLE", 0, 10), lambda: commands.fault("SILENCE", 3, 121),
                lambda: commands.fault("REBOOT", 3, 10), lambda: commands.fault_load(95, 10),
                lambda: commands.metrics("maybe")):
        with pytest.raises(ValueError):
            bad()
    security = SecurityManager("presence", tmp_path / "missing.json")
    assert security.command_requires_auth("FAULT POWER_CYCLE 3 20")
    assert security.command_requires_auth("FAULT LOAD 40 10 HIGH")
    assert security.command_requires_auth(f"WIRELESS POLICY 2 {UUID} AUTO")
    assert not security.command_requires_auth("METRICS RESET")
    # Cada ensaio do catálogo gera um comando que o firmware aceita.
    for experiment in EXPERIMENTS:
        command = experiment.command(3, experiment.default_seconds, 40)
        assert (command is None) == (experiment.kind == "MANUAL")
        assert all(name in MEASURE_LABELS for name in experiment.measures)
        if command:
            assert re.fullmatch(r"FAULT (POWER_CYCLE|SILENCE|AP_OUTAGE) 3 \d+|FAULT DROP_SESSION 3|FAULT LOAD 40 \d+ (HIGH|LOW)", command)


# --------------------------------------------------------------------------
# 5. Ponta a ponta com as gravações da bancada virtual
# --------------------------------------------------------------------------


def _replay(name: str):
    """Entrega uma gravação ao decodificador com o relógio da própria gravação."""

    now = [0.0]
    _bus, state, router, events, metrics, recorder = _new_domain(clock=lambda: now[0])
    path = REPLAYS / f"{name}.log"

    async def run() -> None:
        for raw in path.read_text(encoding="utf-8").splitlines():
            if not raw.startswith("@"):
                continue
            stamp, _, line = raw[1:].partition(" ")
            now[0] = int(stamp) / 1000.0
            await router.decode(line)
            recorder.poll()

    asyncio.run(run())
    return state, events, metrics, recorder, now


def test_replays_cover_every_bench_scenario() -> None:
    names = sorted(path.stem for path in REPLAYS.glob("*.log"))
    assert names == sorted(SCENARIOS)
    # As gravações precisam estar no repositório: "*.log" é ignorado e a
    # exceção em .gitignore é o que as mantém versionadas.
    assert "!Front/replays/*.log" in (ROOT / ".gitignore").read_text(encoding="utf-8").splitlines()
    for path in REPLAYS.glob("*.log"):
        text = path.read_text(encoding="utf-8")
        assert text.startswith(f"# cenario={path.stem}:")
        assert text.rstrip().splitlines()[-1].endswith("falhas=0")
        # Nenhuma chave, senha ou segredo trafega nas linhas da Probe.
        assert "000102030405" not in text and "psk" not in text.lower()


@needs_cxx
def test_replays_match_a_fresh_run_of_the_virtual_bench(tmp_path: Path) -> None:
    """As gravações versionadas são exatamente o que o firmware atual produz."""

    subprocess.run([str(CODE / "scripts/record_network_scenarios.sh"), str(tmp_path)], check=True, capture_output=True)
    assert sorted(path.stem for path in tmp_path.glob("*.log")) == sorted(SCENARIOS)
    for name in SCENARIOS:
        fresh = (tmp_path / f"{name}.log").read_text(encoding="utf-8")
        assert fresh == (REPLAYS / f"{name}.log").read_text(encoding="utf-8"), (
            f"{name}.log desatualizado; execute Codigo/scripts/record_network_scenarios.sh"
        )


def test_formation_replay_feeds_bus_metrics_and_response_time_analysis() -> None:
    state, _events, metrics, _recorder, _now = _replay("formation")
    snapshot = metrics.snapshot()
    assert snapshot.has_bus_data and snapshot.error_state == "ERROR_ACTIVE"
    assert 0 < snapshot.max_load_percent < 2.0 and snapshot.max_peak_percent >= snapshot.max_load_percent
    assert snapshot.coordination.leader == 3 and snapshot.coordination.heartbeat_period_ms == 1000
    assert snapshot.coordination.elections == 1 and snapshot.coordination.leader_changes == 0
    heartbeat = next(item for item in snapshot.ids if item.can_id == 0x100)
    assert heartbeat.dlc == 5 and abs(heartbeat.dt_avg_us - 1_000_000) < 3000
    assert snapshot.analysis is not None and snapshot.analysis.schedulable
    assert snapshot.analysis.utilization < 0.01
    assert snapshot.capacity.extra_sensors > 350 and snapshot.capacity_guideline.extra_sensors > 150
    assert snapshot.capacity_guideline.extra_sensors < snapshot.capacity.extra_sensors
    assert state.snapshot().network.utilization_percent is not None
    assert {1, 2, 3} <= set(state.snapshot().nodes)


def test_sensor_replay_reports_link_round_trip_and_command_latency() -> None:
    state, _events, metrics, recorder, _now = _replay("sensor")
    snapshot = metrics.snapshot()
    link = next(item for item in snapshot.links if item.logical_id == "01.01")
    assert link.link_state == "SECURE" and link.sessions == 1
    assert link.rtt_ms is not None and 0 <= link.rtt_ms < 20 and link.rtt_max_ms >= link.rtt_ms
    assert link.telemetry_count >= 12 and link.telemetry_lost == 0
    assert link.interval_avg_s == pytest.approx(1.0, abs=0.05)
    assert snapshot.commands.count == 1 and snapshot.commands.maximum < 200
    data = next(result for result in snapshot.analysis.results if result.flow.can_id == 0x381)
    assert data.flow.burst >= 9 and data.schedulable
    assert state.find_sensor("01.01").failover_policy == "AUTO"
    assert recorder.active is None and not recorder.history     # nenhum ensaio nesta gravação


def test_owner_failover_replay_is_measured_without_operator_input() -> None:
    state, events, metrics, recorder, _now = _replay("owner_failover")
    recorder.stop()
    run = recorder.history[0]
    # O ensaio foi reconhecido pelas mensagens da Probe, não iniciado pela tela.
    assert run.experiment.key == "node_power_cycle" and run.origin == "OBSERVED"
    assert run.node_id == 1 and run.seconds == 40
    measured = run.metrics
    assert measured["node_fault_s"] == pytest.approx(5.0, abs=1.5)
    assert measured["failover_decision_s"] == pytest.approx(5.2, abs=1.5)
    assert measured["failovers"] == 1 and measured["nodes_in_fault"] == {1}
    assert 15.0 < measured["session_recovered_s"] < 35.0
    assert 15.0 < measured["telemetry_gap_max_s"] < 35.0
    assert measured["node_rejoined_s"] == pytest.approx(43.2, abs=2.0)
    assert measured["telemetry_lost"] == 0 and measured["heartbeat_gap_max_s"] < 1.5
    assert any("reassociado: Módulo 01 → Módulo 02" in event.text for event in run.events)
    assert dict(run.rows())["Reassociações decididas"] == "1"
    assert state.find_sensor("01.01") is None and state.find_sensor("02.01").has_data_plane
    assert len(events[WirelessFailoverReceived]) == 1
    assert metrics.snapshot().coordination.failovers == 1


def test_leader_failure_replay_measures_detection_and_recovery() -> None:
    _state, _events, metrics, recorder, _now = _replay("leader_failure")
    recorder.stop()
    run = recorder.history[0]
    assert run.experiment.key == "leader_power_cycle" and run.leader_before == 3 and run.node_id == 3
    measured = run.metrics
    # A Probe percebe a falta após 4 períodos; eleição (5 s), recuperação (1,5 s) e primeiro sinal (1 s).
    assert measured["leader_detect_s"] == pytest.approx(4.0, abs=0.3)
    assert measured["leader_recovered_s"] == pytest.approx(11.5, abs=0.5)
    assert measured["heartbeat_gap_max_s"] == pytest.approx(12.0, abs=1.0)
    assert measured["elections"] == 1 and measured["leader_after"] == 2 and measured["leaders_seen"] == {2, 3}
    assert measured["node_rejoined_s"] == pytest.approx(23.2, abs=2.0)
    assert measured["nodes_in_fault"] == set()
    coordination = metrics.snapshot().coordination
    assert coordination.leader == 2 and coordination.leader_changes == 1 and coordination.leader_failures == 1


def test_followers_recover_the_network_when_the_probe_cannot_transmit() -> None:
    _state, events, metrics, recorder, _now = _replay("leader_failure_no_probe")
    recorder.stop()
    measured = recorder.history[0].metrics
    kinds = [event.kind for event in events[NetworkEventReceived]]
    # A Probe detecta a falta, mas o pedido dela não chega ao barramento: a
    # eleição que elege o novo líder é a pedida pelo seguidor.
    assert "LEADER_FAILURE_DETECTED" in kinds and kinds.count("ELECTION_REQUESTED") >= 2
    assert measured["leader_after"] == 2
    assert 12.0 < measured["leader_recovered_s"] < 16.0
    assert metrics.snapshot().coordination.leader == 2


def test_leader_freeze_replay_ends_with_a_single_leader_and_no_false_faults() -> None:
    _state, _events, metrics, recorder, _now = _replay("silence_leader")
    recorder.stop()
    run = recorder.history[0]
    assert run.experiment.key == "node_silence" and run.node_id == 3
    assert run.metrics["leaders_seen"] == {2, 3} and run.metrics["leader_after"] == 3
    assert run.metrics["nodes_in_fault"] == set()
    assert any("falha encerrada" in event.text for event in run.events)
    assert metrics.snapshot().coordination.nodes_in_fault == set()


def test_access_point_and_session_experiments_measure_reauthentication() -> None:
    for name, key in (("ap_outage", "ap_outage"), ("drop_session", "drop_session")):
        _state, _events, metrics, recorder, _now = _replay(name)
        if recorder.active is not None:
            recorder.stop()
        # O primeiro ensaio da gravação (a lista guarda o mais recente na frente).
        run = [item for item in recorder.history if item.experiment.key == key][-1]
        measured = run.metrics
        assert measured["session_down_s"] < 2.0, name
        assert 5.0 < measured["session_recovered_s"] < 20.0, name
        assert measured["failovers"] == 0, name                # interrupção curta: o sensor fica onde está
        assert measured["telemetry_gap_max_s"] < 25.0, name
        link = metrics.snapshot().links[0]
        assert link.sessions == 2 and link.last_recovery_s == pytest.approx(measured["session_recovered_s"], abs=1.5), name


def test_bus_load_replay_shows_measured_occupation_and_no_missed_deadline() -> None:
    _state, _events, metrics, recorder, _now = _replay("bus_load")
    if recorder.active is not None:
        recorder.stop()
    runs = {run.experiment.key: run for run in recorder.history}
    high = runs["bus_load_high"]
    assert high.load_percent == 40 and high.seconds == 10
    assert 35.0 < high.metrics["load_max"] < 48.0 and high.metrics["peak_max"] >= high.metrics["load_max"]
    assert high.metrics["heartbeat_gap_max_s"] < 1.2 and high.metrics["telemetry_lost"] == 0
    assert high.metrics["elections"] == 0 and high.metrics["tx_err_max"] == 0
    assert any("Carga efetivamente gerada" in event.text for event in high.events)
    assert "bus_load_low" in runs and runs["bus_load_low"].load_percent == 30
    assert metrics.snapshot().max_load_percent > 35.0


def test_metrics_export_writes_json_and_csv(tmp_path: Path) -> None:
    _state, _events, metrics, recorder, _now = _replay("owner_failover")
    recorder.stop()
    paths = export_metrics(metrics.snapshot(), list(recorder.history), tmp_path)
    assert [path.suffix for path in paths] == [".json", ".csv", ".csv"]
    payload = json.loads(paths[0].read_text(encoding="utf-8"))
    assert payload["bitrate"] == 500_000 and payload["coordination"]["failovers"] == 1
    assert payload["experiments"][0]["experiment"] == "node_power_cycle"
    assert payload["experiments"][0]["metrics"]["nodes_in_fault"] == [1]
    assert payload["response_time_analysis"]["method"].startswith("Davis")
    assert any(row["id"] == "0x100" for row in payload["ids"])
    ids_csv = paths[1].read_text(encoding="utf-8").splitlines()
    assert ids_csv[0].startswith("id,mensagem,grupo") and any(line.startswith("0x100,") for line in ids_csv)
    bus_csv = paths[2].read_text(encoding="utf-8").splitlines()
    assert bus_csv[0].startswith("t_s,janela_ms") and len(bus_csv) > 30
    assert json.dumps(snapshot_to_dict(metrics.snapshot()))          # serializável sem ensaios


# --------------------------------------------------------------------------
# 6. Telas
# --------------------------------------------------------------------------


def _app(tmp_path: Path, **kwargs) -> PicoTuiApp:
    return PicoTuiApp(enable_file_log=False, security_mode="off", preferences_path=tmp_path / "tui.json", **kwargs)


def _screen_text(app: PicoTuiApp) -> str:
    parts = []
    for widget in app.screen.walk_children(with_self=False):
        if isinstance(widget, WrappedLines):
            parts.append(widget.source_text())
    return " ".join(parts)


async def test_metrics_screen_shows_demo_measurements(tmp_path: Path) -> None:
    app = _app(tmp_path, demo=True)
    async with app.run_test(size=(120, 50)) as pilot:
        await settle(pilot, 10)
        await pilot.press("b")
        await settle(pilot, 4)
        assert isinstance(app.screen, MetricsScreen)
        text = _screen_text(app)
        assert "Ocupação (1 s)" in text and "Módulo 23" in text
        assert "Davis et al. (2007)" in text and "Todos os prazos cumpridos" in text
        assert "0x100" in text and "Sinal de presença do líder" in text
        assert "sensores sem fio" in text                               # capacidade restante
        await pilot.press("z")
        await settle(pilot, 2)
        assert app.metrics.snapshot().total_frames < 200
        await pilot.press("f9")
        await settle(pilot, 3)
        assert isinstance(app.screen, ExperimentsScreen)
        await pilot.press("f8")
        await settle(pilot, 3)
        assert isinstance(app.screen, MetricsScreen)


async def test_experiment_flow_asks_target_duration_and_confirmation(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.chdir(tmp_path)
    app = _app(tmp_path, demo=True)
    async with app.run_test(size=(120, 50)) as pilot:
        await settle(pilot, 6)
        await pilot.press("e")
        await settle(pilot, 3)
        assert isinstance(app.screen, ExperimentsScreen)
        assert "Queda do líder" in _screen_text(app) or app.screen.query_one("#exp-list").option_count == len(EXPERIMENTS)
        await pilot.press("enter")                    # Queda do líder
        await settle(pilot, 3)
        assert isinstance(app.screen, ChoiceScreen)   # módulo: o líder vem realçado
        choices = app.screen.query_one("#choice-list")
        assert str(choices.get_option_at_index(choices.highlighted).id) == "23"
        await pilot.press("enter")
        await settle(pilot, 3)
        assert isinstance(app.screen, ParameterScreen)
        await pilot.press("enter")                    # duração padrão
        await settle(pilot, 3)
        assert isinstance(app.screen, ConfirmScreen)
        assert "FAULT POWER_CYCLE 23 20" in app.screen.message
        await pilot.press("s")
        await settle(pilot, 4)
        run = app.experiments.active
        assert run is not None and run.experiment.key == "leader_power_cycle" and run.node_id == 23
        assert isinstance(app.screen, ExperimentsScreen)
        assert "Queda do líder, Módulo 23, 20 s" in _screen_text(app)
        # Um segundo ensaio não começa enquanto o primeiro está em curso.
        app.start_experiment("node_silence")
        await settle(pilot, 3)
        assert isinstance(app.screen, ExperimentsScreen) and app.experiments.active is run
        await pilot.press("s")                        # encerra
        await settle(pilot, 3)
        assert app.experiments.active is None and app.experiments.history[0] is run
        assert run.state == "STOPPED"
        assert "Queda do líder" in " ".join(entry.message for entry in app.message_history)
        await pilot.press("x")
        await settle(pilot, 3)
        exported = sorted(path.name for path in (tmp_path / "exports").iterdir())
        assert len(exported) == 3 and exported[0].startswith("metricas_")


async def test_experiments_need_the_probe_and_cancelled_dialogs_send_nothing(tmp_path: Path) -> None:
    app = _app(tmp_path)                              # sem conexão
    async with app.run_test(size=(100, 40)) as pilot:
        await settle(pilot, 4)
        app.start_experiment("bus_load_high")
        await settle(pilot, 3)
        assert app.experiments.active is None and not isinstance(app.screen, (ChoiceScreen, ParameterScreen))

    app = _app(tmp_path, demo=True)
    async with app.run_test(size=(100, 40)) as pilot:
        await settle(pilot, 6)
        app.start_experiment("bus_load_high")
        await settle(pilot, 3)
        assert isinstance(app.screen, ParameterScreen)
        await pilot.press("escape")
        await settle(pilot, 3)
        assert app.experiments.active is None
        app.start_experiment("bus_load_low")
        await settle(pilot, 3)
        await pilot.press("enter")                    # duração
        await settle(pilot, 3)
        assert isinstance(app.screen, ChoiceScreen)   # percentual de carga
        await pilot.press("enter")
        await settle(pilot, 3)
        assert isinstance(app.screen, ConfirmScreen) and "FAULT LOAD 40 15 LOW" in app.screen.message
        await pilot.press("n")
        await settle(pilot, 3)
        assert app.experiments.active is None


async def test_replay_mode_drives_the_screens_and_never_transmits(tmp_path: Path) -> None:
    app = _app(tmp_path, replay=REPLAYS / "owner_failover.log", replay_speed=60.0)
    async with app.run_test(size=(48, 18)) as pilot:
        await settle(pilot, 10)                       # cerca de 70 s de gravação
        assert app.replaying and app.serial_client is None
        assert app._send_raw("FAULT POWER_CYCLE 1 5")           # aceito, mas não enviado
        await settle(pilot, 2)
        assert any("não enviado" in entry.message for entry in app.message_history)
        assert app.experiments.active is not None or app.experiments.history
        await pilot.press("e")
        await settle(pilot, 4)
        assert isinstance(app.screen, ExperimentsScreen)
        text = _screen_text(app)
        assert "Queda de um módulo, Módulo 01, 40 s" in text
        assert "Reassociações decididas: 1" in text or "reassociado: Módulo 01 → Módulo 02" in text
        # Mesmo no menor terminal suportado, nada é cortado na tela de ensaios em andamento.
        for widget in app.screen.walk_children(with_self=False):
            if isinstance(widget, WrappedLines) and widget.display and widget.region.width:
                width = widget.content_size.width
                assert all(line.cell_len <= width for line in widget._lines_for(width)), widget.id
        await pilot.press("b")
        await settle(pilot, 4)
        assert isinstance(app.screen, MetricsScreen)
        snapshot = app.metrics.snapshot()
        assert snapshot.has_bus_data and snapshot.coordination.failovers == 1
        # As durações medidas são as da gravação, não as do relógio da máquina.
        run = app.experiments.active or app.experiments.history[0]
        assert run.metrics["failover_decision_s"] == pytest.approx(5.2, abs=1.5)


async def test_association_dialog_offers_automatic_reassociation(tmp_path: Path) -> None:
    app = _app(tmp_path, replay=REPLAYS / "manual_policy.log", replay_speed=200.0)
    async with app.run_test(size=(110, 40)) as pilot:
        await settle(pilot, 3)
        sensor = app.state_store.find_sensor("01.01")
        assert sensor is not None and sensor.failover_policy == "MANUAL"
        app.request_wireless_policy("01.01")
        await settle(pilot, 3)
        assert isinstance(app.screen, ChoiceScreen)
        choices = app.screen.query_one("#choice-list")
        assert str(choices.get_option_at_index(choices.highlighted).id) == "MANUAL"
        await pilot.press("up")
        await pilot.press("enter")
        await settle(pilot, 3)
        assert any(f"WIRELESS POLICY 1 {UUID} AUTO" in entry.message for entry in app.message_history)


@pytest.mark.parametrize(
    "command",
    [
        "22 00 FF 01",
        "22 0 FF 01",
        "0x22 00 FF 01",
        "22 0 ff 1",
        "122 100 FF 1",       # %hhx mantém só o byte menos significativo
        "22+10 5 44",         # o sinal também separa campos para o sscanf
        "-DE 0 FF 1",         # -0xDE truncado a um byte é 0x22
        "22 0x10 3 0x44 lixo",
        "22\t30\t1\t5",
    ],
)
def test_can_commands_need_authorization_however_they_are_spelled(command: str) -> None:
    from pico_tui.security import SecurityManager

    assert SecurityManager(mode="presence").command_requires_auth(command)


@pytest.mark.parametrize("command", ["22 20 FF 01", "22 20 ff 1", "0x22 0x20 0xFF 1", "STATUS", "METRICS RESET"])
def test_read_only_commands_stay_free(command: str) -> None:
    from pico_tui.security import SecurityManager

    assert not SecurityManager(mode="presence").command_requires_auth(command)


# --------------------------------------------------------------------------
# 7. Correções vindas da revisão
# --------------------------------------------------------------------------

_TEL = ("TEL NODE={node} CHILD=1 SEQ={seq} GW_TS_MS=1 MODE=ROTATING ACQ=POLLING WIN=512 AXIS=VECTOR FFT_VALID=NO "
        "RATE_REQ_HZ=1000.00 RATE_EFF_HZ=1000.00 RMS=0.1 RMS_UNIT=m/s2 KURT=0 CREST=2 PEAK_HZ=31 PEAK_AMP=0.02 ENT=0.4 "
        "PPV_MM_S=0.4 STA_LTA=NO CLIP=NO BATT_PCT=93 BATT_MV=3980 DTC=0x0000 DTC_COUNT=0")


def _timed(router: DecoderRouter, recorder: ExperimentRecorder, now: list[float], script) -> None:
    async def run() -> None:
        for at, line in script:
            now[0] = at
            await router.decode(line)
            recorder.poll()

    asyncio.run(run())


def test_telemetry_gap_counts_from_the_last_sample_before_the_fault() -> None:
    """O intervalo sem telemetria não depende de haver uma amostra entre o
    início do ensaio e a falha, nem da ordem em que as linhas chegam."""

    now = [0.0]
    _bus, _state, router, _events, _metrics, recorder = _new_domain(clock=lambda: now[0])
    script = [(float(second), _TEL.format(node=1, seq=second)) for second in range(1, 6)]
    script += [
        (5.2, "[GW] FAULT_TX kind=POWER_CYCLE node=1 duration_ms=40000 seq=1"),
        (5.21, "[GW] FAULT_EVENT node=1 kind=POWER_CYCLE state=STARTED duration_ms=40000 seq=1"),
        (10.4, f"[GW] WIRELESS_FAILOVER uuid={UUID} from=1 to=2 reason=OWNER_LOST rssi=-60"),
        (29.0, _TEL.format(node=2, seq=0)),
        (30.0, _TEL.format(node=2, seq=1)),
    ]
    _timed(router, recorder, now, script)
    run = recorder.stop()
    assert run is not None
    assert run.metrics["telemetry_gap_max_s"] == pytest.approx(24.0)      # de 5,0 s (antes do ensaio) a 29,0 s
    assert run.metrics["telemetry_lost"] == 0                              # nova contagem no novo módulo
    assert any("passou a chegar como 02.01" in event.text for event in run.events)
    # A queda da sessão não foi relatada por ninguém (o módulo estava desligado): não é inventada.
    assert "session_down_s" not in run.metrics
    assert dict(run.rows())["Sessão autenticada novamente em"] != "não observado"


def test_sensors_already_silent_do_not_enter_the_measurement() -> None:
    now = [0.0]
    _bus, _state, router, _events, _metrics, recorder = _new_domain(clock=lambda: now[0])
    script = [(1.0, _TEL.format(node=1, seq=1)), (2.0, _TEL.format(node=1, seq=2))]
    # O sensor já estava calado havia 28 s quando o ensaio começou.
    script += [
        (30.0, "[GW] FAULT_TX kind=POWER_CYCLE node=2 duration_ms=20000 seq=1"),
        (30.01, "[GW] FAULT_EVENT node=2 kind=POWER_CYCLE state=STARTED duration_ms=20000 seq=1"),
        (45.0, _TEL.format(node=3, seq=0)),
    ]
    _timed(router, recorder, now, script)
    now[0] = 45.5
    run = recorder.stop()
    assert run is not None
    assert "telemetry_gap_max_s" not in run.metrics
    assert dict(run.rows())["Maior intervalo sem telemetria"] == "não observado"
    assert not any("não retomada" in event.text for event in run.events)


def test_telemetry_not_resumed_is_reported_at_the_end_of_the_experiment() -> None:
    now = [0.0]
    _bus, _state, router, _events, _metrics, recorder = _new_domain(clock=lambda: now[0])
    script = [(float(second), _TEL.format(node=1, seq=second)) for second in range(1, 4)]
    script += [(3.5, "[GW] FAULT_EVENT node=1 kind=POWER_CYCLE state=STARTED duration_ms=60000 seq=1")]
    _timed(router, recorder, now, script)
    now[0] = 33.5
    run = recorder.stop()
    assert run is not None and run.metrics["telemetry_gap_max_s"] == pytest.approx(30.5)
    assert any("telemetria não retomada" in event.text for event in run.events)


def test_failover_retry_replays_end_with_the_sensor_on_a_working_node() -> None:
    # O Node escolhido perde o sensor: segunda decisão depois do intervalo mínimo.
    state, events, metrics, _recorder, _now = _replay("failover_retry")
    decisions = events[WirelessFailoverReceived]
    assert [(item.from_node_id, item.to_node_id) for item in decisions] == [(1, 2), (2, 3)]
    assert state.find_sensor("03.01").has_data_plane
    assert state.find_sensor("01.01") is None and state.find_sensor("02.01") is None
    assert metrics.snapshot().coordination.failovers == 2

    # O Node escolhido sai do ar: o vínculo anterior é mantido até a nova decisão.
    state, events, metrics, recorder, _now = _replay("failover_dead_target")
    decisions = events[WirelessFailoverReceived]
    assert [(item.from_node_id, item.to_node_id) for item in decisions] == [(1, 2), (1, 3)]
    assert state.find_sensor("03.01").has_data_plane and state.find_sensor("01.01") is None
    recorder.stop()
    run = recorder.history[0]
    assert run.node_id == 2 and run.metrics["nodes_in_fault"] == {2} and run.metrics["failovers"] == 2


def test_leader_resuming_from_a_long_freeze_does_not_reassociate_a_working_sensor() -> None:
    state, events, _metrics, recorder, _now = _replay("silence_leader_with_sensor")
    assert events[WirelessFailoverReceived] == []
    assert state.find_sensor("01.01").has_data_plane
    run = recorder.history[0]
    assert run.experiment.key == "node_silence" and run.metrics["failovers"] == 0
    assert run.metrics["telemetry_gap_max_s"] < 2.0 and run.metrics["leader_after"] == 3


async def test_replay_measurements_use_the_recorded_time_base(tmp_path: Path) -> None:
    app = _app(tmp_path, replay=REPLAYS / "sensor.log", replay_speed=200.0)
    async with app.run_test(size=(100, 40)) as pilot:
        await settle(pilot, 6)
        snapshot = app.metrics.snapshot()
        # A sessão de medição começa no primeiro instante da gravação, não no relógio da máquina.
        assert 0.0 <= app.metrics.started < 1.0
        assert 0.0 <= snapshot.now - snapshot.started < 60.0
        assert all(0.0 <= snapshot.now - event.t < 60.0 for event in app.metrics.timeline)


def test_sensor_network_diagnostics_are_shown_as_messages() -> None:
    """As linhas "[NET] ..." do console do sensor chegam a Mensagens como INFO."""

    from pico_tui.core.events import LogEvent
    from pico_tui.core.models import ConnectionMode

    bus = EventBus()
    logs: list[LogEvent] = []
    bus.subscribe(LogEvent, logs.append)
    router = DecoderRouter(bus, requested_mode="auto")
    _feed(router, "[NET] Wi-Fi IOC-02: conectado, ip=192.168.4.2 gateway=192.168.4.1")
    assert router.mode == ConnectionMode.SENSOR_DIRECT
    assert any(item.level == "INFO" and item.source == "REDE" and "IOC-02" in item.message for item in logs)
