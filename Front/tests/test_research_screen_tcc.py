"""Tela de pesquisa ("/") e lançador de comandos ("k").

As linhas vêm da bancada virtual (cenário ``pesquisa`` de sim_network.cpp,
com eco LOCAL e SENSOR, carga MID de 30 % e semente 7).
"""
from __future__ import annotations

import asyncio
import importlib.util
import random
import sys
from pathlib import Path

import pytest
from textual.widgets import Input

from pico_tui.app import PicoTuiApp
from pico_tui.command_catalog import describe_raw_command
from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import LegacyHeartbeatReceived, ResearchLineReceived
from pico_tui.dialogs import CommandLauncher
from pico_tui.protocol.router import DecoderRouter
from pico_tui.research import stats
from pico_tui.research.service import ResearchService
from pico_tui.research_screen import ResearchScreen
from pico_tui.screens import HomeScreen
from tests.conftest import settle

ROOT = Path(__file__).resolve().parents[2]

LINES = """\
@301 [GW] CONFIG node=0 bitrate=500000 hb_period_ms=1000 assoc_stale_ms=15000 assoc_lost_ms=45000 assoc_hysteresis_ms=0 stream_period_ms=1000 ping_ms=3000 session_timeout_ms=10000 offer_hold_ms=15000 load_ids=0x010,0x1F0,0x7EF fault_injection=1 follower_watchdog=1 t_us=300037
@1301 [GW] BUS_STATS t_ms=1300 win_ms=1000 frames=4 bits=264 load=0.05 peak=0.52 rx_err=0 tx_err=0 eflg=0x00 rx_peak=3 ids=2 xfer_ok=0 xfer_err=0 own=1 untracked=0 t_us=1300037
@8701 [GW] HEARTBEAT lider=3 rodada=0 sensores_ativos=2 modo=3 periodo=1000 ms t_us=8700574
@9701 [GW] HEARTBEAT lider=3 rodada=1 sensores_ativos=2 modo=3 periodo=1000 ms t_us=9700537
@10701 [GW] HEARTBEAT lider=3 rodada=2 sensores_ativos=2 modo=3 periodo=1000 ms t_us=10700537
@15503 [GW] WIRELESS_STATS node=1 child=1 crypto_rx_us=37 crypto_tx_us=37 crypto_max_us=37 handshake_us=148 bridge_avg_us=1000 bridge_max_us=1000 t_us=15502574
@20704 [GW] FAULT_EVENT node=0 kind=BUS_LOAD state=STARTED duration_ms=20000 seq=1 load_pct=30 priority=MID frames=0 achieved_pct=0.00 seed=7 seq_hash=0x811C9DC5 t_us=20703037
@20704 [GW] ECHO_START node=1 child=1 path=LOCAL count=4 period_ms=300 sampling=POISSON seed=7 t_us=20703537
@21031 [GW] ECHO node=1 child=1 path=LOCAL n=1 seq=1 status=APPLIED rtt_us=1574 wl_us=-1 t_us=21030185
@21747 [GW] ECHO node=1 child=1 path=LOCAL n=2 seq=2 status=APPLIED rtt_us=1537 wl_us=-1 t_us=21746148
@21865 [GW] ECHO node=1 child=1 path=LOCAL n=3 seq=3 status=TIMEOUT rtt_us=-1 wl_us=-1 t_us=21864185
@21895 [GW] ECHO node=1 child=1 path=LOCAL n=4 seq=4 status=APPLIED rtt_us=1600 wl_us=-1 t_us=21894148
@23545 [GW] ECHO_DONE node=1 child=1 path=LOCAL sent=4 received=3 lost=1 min_us=1537 avg_us=1570 max_us=1600 t_us=23544611
@23604 [GW] ECHO_START node=1 child=1 path=SENSOR count=10 period_ms=300 sampling=POISSON seed=8 t_us=23603037
@23635 [GW] ECHO node=1 child=1 path=SENSOR n=1 seq=11 status=APPLIED rtt_us=2574 wl_us=600 t_us=23634685
@23719 [GW] ECHO node=1 child=1 path=SENSOR n=2 seq=12 status=APPLIED rtt_us=2774 wl_us=800 t_us=23718648
@24000 [GW] BENCH domain=probe op=HMAC_DATAGRAM bytes=64 n=200 mean_us=41.50 min_us=40 p50_us=41 p95_us=44 p99_us=47 max_us=52
@24001 [GW] BENCH_DONE ops=1 cpu_mhz=240 t_us=24000000
"""


def _decoded(lines: str) -> ResearchService:
    async def scenario() -> ResearchService:
        bus = EventBus()
        service = ResearchService(bus)
        router = DecoderRouter(bus, "gateway")
        for line in lines.splitlines():
            await router.decode(line.split(" ", 1)[1])
        return service

    return asyncio.run(scenario())


def test_probe_lines_become_research_events() -> None:
    async def scenario() -> None:
        bus = EventBus()
        seen: list = []

        async def collect(event) -> None:
            seen.append(event)

        bus.subscribe(ResearchLineReceived, collect)
        bus.subscribe(LegacyHeartbeatReceived, collect)
        router = DecoderRouter(bus, "gateway")
        for line in LINES.splitlines():
            await router.decode(line.split(" ", 1)[1])
        kinds = [event.kind for event in seen if isinstance(event, ResearchLineReceived)]
        assert kinds[:2] == ["CONFIG", "WIRELESS_STATS"] and "ECHO_DONE" in kinds and kinds[-1] == "BENCH_DONE"
        heartbeats = [event for event in seen if isinstance(event, LegacyHeartbeatReceived)]
        assert [event.t_us for event in heartbeats] == [8700574, 9700537, 10700537]
        config = next(event for event in seen if isinstance(event, ResearchLineReceived) and event.kind == "CONFIG")
        assert config.fields["ASSOC_STALE_MS"] == "15000" and config.fields["LOAD_IDS"] == "0x010,0x1F0,0x7EF"

    asyncio.run(scenario())


def test_service_summarizes_echo_trains_and_node_costs() -> None:
    snapshot = _decoded(LINES).snapshot()
    local = snapshot.latest_train("LOCAL")
    assert local is not None and not local.active
    assert (local.sent, local.received, local.lost) == (4, 3, 1)
    summary = local.rtt()
    assert summary.n == 3 and summary.p50 == 1574 and summary.maximum == 1600
    assert local.loss()[0] == pytest.approx(0.25)
    sensor = snapshot.latest_train("SENSOR")
    assert sensor is not None and sensor.active and sensor.sent == 2
    assert sensor.wifi().p50 == pytest.approx(700) and sensor.can_part().p50 == pytest.approx(1974)
    assert snapshot.heartbeat_intervals_us == pytest.approx([999963.0, 1000000.0])
    assert snapshot.links[0].logical_id == "01.01" and snapshot.links[0].last_handshake_us() == 148
    assert snapshot.load is not None and snapshot.load.fields["PRIORITY"] == "MID"
    assert snapshot.bench[0].op == "HMAC_DATAGRAM" and snapshot.bench_cpu_mhz == 240
    assert snapshot.config is not None and snapshot.config["HB_PERIOD_MS"] == "1000"
    assert snapshot.bus_loads == [0.05]


def test_leader_change_does_not_count_the_election_as_jitter() -> None:
    async def scenario() -> ResearchService:
        bus = EventBus()
        service = ResearchService(bus)
        for leader, t_us in ((3, 1_000_000), (3, 2_000_000), (2, 9_000_000), (2, 10_000_000)):
            await bus.publish(LegacyHeartbeatReceived(leader, 0, 0, 3, 1000, t_us=t_us))
        return service

    service = asyncio.run(scenario())
    assert list(service.heartbeat_intervals) == [1_000_000.0, 1_000_000.0]


def _load_research_statistics():
    path = ROOT / "Pesquisa" / "iocpesquisa" / "estatistica.py"
    if not path.exists():
        pytest.skip("Pesquisa/ ausente")
    spec = importlib.util.spec_from_file_location("iocpesquisa_estatistica", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_tui_statistics_match_the_research_toolkit() -> None:
    toolkit = _load_research_statistics()
    rng = random.Random(20261030)
    for n in (1, 2, 5, 31, 200):
        values = [rng.lognormvariate(7.3, 0.4) for _ in range(n)]
        mine, theirs = stats.summarize(values), toolkit.summary(values)
        for attr, key in (("mean", "media"), ("p50", "p50"), ("p95", "p95"), ("p99", "p99"), ("ci_low", "ic95_inf")):
            a, b = getattr(mine, attr), theirs[key]
            assert (a != a and b != b) or a == pytest.approx(b), (n, attr)
    for successes, n in ((0, 30), (3, 40), (40, 40)):
        assert stats.wilson(successes, n) == pytest.approx(toolkit.wilson(successes, n))


def test_launcher_describes_what_will_be_sent() -> None:
    assert "eleição" in describe_raw_command("22 00 ff 01").lower()
    assert "Módulo 04" in describe_raw_command("22 10 04 11")
    assert "sem ação correspondente" in describe_raw_command("22 00 FF 11")
    assert "trem de eco" in describe_raw_command("ECHO 01.01 40 500 LOCAL 7")
    assert "interno da TUI" in describe_raw_command(":tel on")
    assert "sem alteração" in describe_raw_command("STATUS")


def demo_app(prefs_file) -> PicoTuiApp:
    return PicoTuiApp(demo=True, enable_file_log=False, security_mode="off", preferences_path=prefs_file)


async def test_slash_opens_research_screen_hidden_from_the_key_bar(prefs_file) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot, 5)
        assert isinstance(app.screen, HomeScreen)
        bar = app.screen.query_one("#key-bar").source_text()
        assert "Pesquisa" not in bar and "Enviar comando" not in bar
        await pilot.press("slash")
        await settle(pilot, 3)
        assert isinstance(app.screen, ResearchScreen)
        assert "ECHO 01.01 40 500 LOCAL 7" in app.screen.query_one("#rs-commands").source_text()
        await pilot.press("escape")
        await settle(pilot, 2)
        assert isinstance(app.screen, HomeScreen)


async def test_k_launcher_sends_and_remembers_commands(prefs_file) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(120, 40)) as pilot:
        await settle(pilot, 5)
        await pilot.press("k")
        await settle(pilot, 2)
        assert isinstance(app.screen, CommandLauncher)
        await pilot.press(*"22 20 FF 00")
        await settle(pilot, 1)
        hint = str(app.screen.query_one("#launcher-hint").render())
        assert "Comando CAN 22 20 FF 00" in hint and "Solicitar estado de todos os módulos" in hint
        await pilot.press("enter")
        await settle(pilot, 3)
        assert isinstance(app.screen, HomeScreen)
        assert app.command_history == ["22 20 FF 00"]
        assert "22 20 FF 00" in (app.state_store.snapshot().last_action or "")
        # Esc fecha sem enviar; a seta para cima traz o último comando.
        await pilot.press("k")
        await settle(pilot, 2)
        await pilot.press("up")
        await settle(pilot, 1)
        assert app.screen.query_one("#launcher-input", Input).value == "22 20 FF 00"
        await pilot.press("escape")
        await settle(pilot, 2)
        assert isinstance(app.screen, HomeScreen) and app.command_history == ["22 20 FF 00"]


async def test_research_screen_shows_a_replayed_experiment(prefs_file, tmp_path) -> None:
    replay = tmp_path / "pesquisa.log"
    replay.write_text("# cenario=pesquisa\n" + LINES, encoding="utf-8")
    app = PicoTuiApp(replay=str(replay), replay_speed=1000.0, enable_file_log=False, security_mode="off",
                     preferences_path=prefs_file)
    async with app.run_test(size=(140, 50)) as pilot:
        await settle(pilot, 20)
        app.action_show_research()
        await settle(pilot, 3)
        screen = app.screen
        assert isinstance(screen, ResearchScreen)
        local = screen.query_one("#rs-local").source_text()
        assert "1 de 4" in local and "Sensor 01.01" in local and "Poisson" in local
        sensor = screen.query_one("#rs-sensor").source_text()
        assert "Parte Wi-Fi (Node)" in sensor
        assert "HMAC na recepção" in screen.query_one("#rs-links").source_text()
        assert "MID" in screen.query_one("#rs-bus").source_text()
        assert "15000 ms" in screen.query_one("#rs-config").source_text()
        assert "HMAC_DATAGRAM" in screen.query_one("#rs-bench").source_text()
