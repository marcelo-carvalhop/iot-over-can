"""Abertura da estação com vínculos gravados nos módulos: sensor desligado não é alerta.

Os módulos guardam os vínculos na NVS e os republicam a cada 10 s. Quem abre a
estação recebe, de uma vez, os sensores que estão ligados e os que ficaram
desligados (STALE/LOST). Os últimos são pré-aprovados e ausentes, não falhas
desta sessão; só o sensor visto vivo e que depois some vira alerta.
"""
from __future__ import annotations

import asyncio

import pytest

from pico_tui import presentation as pres
from pico_tui.app import PicoTuiApp
from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import LogEvent
from pico_tui.core.models import ConnectionMode, ConnectionState, NodeStatus, TelemetrySample
from pico_tui.core.state_store import StateStore
from pico_tui.protocol.legacy_gateway import LegacyGatewayDecoder
from pico_tui.services.controller import DomainController
from tests.conftest import settle

UUID_A = "0xE6616408432B6F39"
UUID_B = "0xAAAA000000000001"
UUID_C = "0xAAAA000000000002"


def assoc(node: int, child: int, uuid: str, state: str, rssi: int = -50) -> str:
    return (
        f"[GW] WIRELESS_ASSOC node={node} child={child} uuid={uuid} profile=VIBRATION "
        f"state={state} rssi={rssi} protocol=6 failover=AUTO session=NONE"
    )


class Rig:
    """Decodificador, controlador e estado reais, sem a interface."""

    def __init__(self) -> None:
        self.bus = EventBus()
        self.state = StateStore()
        DomainController(self.bus, self.state)
        self.decoder = LegacyGatewayDecoder(self.bus)
        self.logs: list[tuple[str, str]] = []
        self.bus.subscribe(LogEvent, self._log)
        # Conectado, para a faixa de estado e a condição geral valerem.
        self.state.set_connection(ConnectionState.CONNECTED, port="COM3", mode=ConnectionMode.GATEWAY_CAN)

    async def _log(self, event: LogEvent) -> None:
        self.logs.append((event.level, event.message))

    async def feed(self, *lines: str) -> None:
        for line in lines:
            assert await self.decoder.decode(line)

    def sensor(self, logical_id: str):
        return self.state.find_sensor(logical_id)

    def condition(self):
        return pres.system_condition(self.state.snapshot())

    def warnings(self) -> list[str]:
        return [message for level, message in self.logs if level in {"WARNING", "ERROR", "CRITICAL"}]


def run(coro):
    return asyncio.run(coro)


def test_sensors_left_off_do_not_raise_alerts_when_the_station_opens() -> None:
    async def scenario() -> None:
        rig = Rig()
        # Uma abertura típica: um sensor ligado e dois pré-aprovados que estão desligados.
        await rig.feed(
            assoc(1, 1, UUID_A, "ONLINE"),
            assoc(2, 1, UUID_B, "LOST", -90),
            assoc(2, 2, UUID_C, "STALE", -80),
        )
        for logical_id in ("02.01", "02.02"):
            sensor = rig.sensor(logical_id)
            assert sensor.preapproved_absent and not sensor.seen_online
            condition = pres.sensor_condition(sensor)
            assert condition.level == pres.Level.NO_DATA and "Pré-aprovado" in condition.summary
        assert not rig.sensor("01.01").preapproved_absent

        summary = rig.condition()
        assert summary.attention == []                       # nada em "Precisa de atenção"
        assert summary.level == pres.Level.NORMAL and summary.headline == "Operação normal"
        assert (summary.sensors_total, summary.sensors_ok, summary.sensors_absent) == (1, 1, 2)
        # Nenhum aviso: a presença deles só aparece como informação.
        assert rig.warnings() == []
        assert any("pré-aprovado" in message for level, message in rig.logs if level == "INFO")

    run(scenario())


def test_stale_to_lost_of_an_absent_sensor_is_only_debug() -> None:
    async def scenario() -> None:
        rig = Rig()
        await rig.feed(assoc(2, 1, UUID_B, "STALE", -80), assoc(2, 1, UUID_B, "LOST", -90))
        assert rig.warnings() == []
        assert [level for level, _ in rig.logs if "pré-aprovado" in _] == ["INFO", "DEBUG"]
        assert pres.attention_items(rig.state.snapshot()) == []

    run(scenario())


def test_a_sensor_seen_online_and_then_lost_is_a_real_failure() -> None:
    async def scenario() -> None:
        rig = Rig()
        await rig.feed(assoc(2, 1, UUID_B, "ONLINE"))
        assert rig.sensor("02.01").seen_online
        await rig.feed(assoc(2, 1, UUID_B, "STALE", -85))
        await rig.feed(assoc(2, 1, UUID_B, "LOST", -90))

        sensor = rig.sensor("02.01")
        assert not sensor.preapproved_absent
        assert pres.sensor_condition(sensor).level == pres.Level.CRITICAL
        assert rig.condition().level == pres.Level.CRITICAL
        assert len(rig.warnings()) == 2                     # STALE e LOST avisam, como antes

    run(scenario())


def test_a_sensor_that_comes_back_online_is_normal_and_a_later_loss_alerts() -> None:
    async def scenario() -> None:
        rig = Rig()
        await rig.feed(assoc(2, 1, UUID_B, "LOST", -90))
        assert rig.condition().sensors_absent == 1

        await rig.feed(assoc(2, 1, UUID_B, "ONLINE", -48))
        summary = rig.condition()
        assert (summary.sensors_total, summary.sensors_ok, summary.sensors_absent) == (1, 1, 0)
        assert pres.association_label(rig.sensor("02.01")) == "ONLINE"

        await rig.feed(assoc(2, 1, UUID_B, "STALE", -85))
        assert pres.sensor_condition(rig.sensor("02.01")).level == pres.Level.ATTENTION

    run(scenario())


def test_a_bind_made_in_this_session_that_fails_is_not_hidden() -> None:
    async def scenario() -> None:
        rig = Rig()
        # ASSOCIATING só aparece quando o operador acaba de pedir o vínculo.
        await rig.feed(assoc(3, 1, UUID_C, "ASSOCIATING"), assoc(3, 1, UUID_C, "BOUND"))
        assert rig.sensor("03.01").seen_online
        await rig.feed(assoc(3, 1, UUID_C, "STALE", -90))
        assert pres.sensor_condition(rig.sensor("03.01")).level == pres.Level.ATTENTION
        assert rig.condition().level == pres.Level.ATTENTION

    run(scenario())


def test_a_restored_binding_stays_neutral_until_it_is_heard() -> None:
    async def scenario() -> None:
        rig = Rig()
        # O módulo retoma o vínculo da NVS como BOUND, sem ter ouvido o sensor.
        await rig.feed(assoc(3, 1, UUID_C, "BOUND", -127))
        sensor = rig.sensor("03.01")
        assert not sensor.seen_online and not sensor.preapproved_absent   # BOUND ainda não é ausência
        assert pres.attention_items(rig.state.snapshot()) == []
        await rig.feed(assoc(3, 1, UUID_C, "STALE", -127))                # 15 s depois, sem ouvi-lo
        assert rig.sensor("03.01").preapproved_absent
        assert pres.attention_items(rig.state.snapshot()) == []

    run(scenario())


def test_telemetry_or_a_secure_session_prove_the_sensor_is_alive() -> None:
    async def scenario() -> None:
        rig = Rig()
        await rig.feed(
            assoc(2, 1, UUID_B, "LOST", -90),
            assoc(2, 2, UUID_C, "STALE", -80),
            "[GW] WIRELESS_LINK node=2 child=2 state=SECURE rssi=-60 age_s=3 rx=4 auth_fail=0 replay=0 lost=0 "
            "stream=ON period_ms=1000 rtt_ms=5 rtt_max_ms=9",
        )
        assert not rig.sensor("02.01").seen_online
        assert rig.sensor("02.02").seen_online and not rig.sensor("02.02").preapproved_absent

        sensor = rig.state.ensure_sensor(2, 1)
        assert sensor.preapproved_absent
        # Qualquer telemetria que chegue marca o sensor como visto.
        rig.state.update_telemetry(TelemetrySample(2, 1, rms=0.2))
        assert rig.sensor("02.01").seen_online and not rig.sensor("02.01").preapproved_absent

    run(scenario())


def test_the_default_selection_prefers_a_sensor_that_is_alive() -> None:
    async def scenario() -> None:
        rig = Rig()
        # O sensor ausente chega primeiro e tem o menor identificador.
        await rig.feed(assoc(1, 1, UUID_B, "LOST", -90))
        assert rig.state.snapshot().selected_logical_id is None   # não virou seleção padrão
        await rig.feed(assoc(2, 1, UUID_A, "ONLINE"))
        assert rig.state.first_sensor_id() == "02.01"

        # Só há sensores ausentes: ainda é possível escolher um deles.
        solo = Rig()
        await solo.feed(assoc(1, 1, UUID_B, "LOST", -90))
        assert solo.state.first_sensor_id() == "01.01"

    run(scenario())


def test_an_explicit_choice_of_the_operator_is_never_undone() -> None:
    async def scenario() -> None:
        rig = Rig()
        await rig.feed(assoc(1, 1, UUID_B, "LOST", -90))
        rig.state.set_selected("01.01")                   # o operador abriu o sensor
        await rig.feed(assoc(1, 1, UUID_B, "STALE", -85))  # outro relato do mesmo vínculo
        assert rig.state.snapshot().selected_logical_id == "01.01"

    run(scenario())


def test_labels_name_the_absent_sensor_instead_of_calling_it_lost() -> None:
    async def scenario() -> None:
        rig = Rig()
        await rig.feed(assoc(2, 1, UUID_B, "LOST", -90))
        sensor = rig.sensor("02.01")
        assert pres.association_label(sensor) == "LOST (pré-aprovado, ausente)"
        assert pres.sensor_status_label(sensor) == "Pré-aprovado, ausente"
        assert pres.sensor_status_label(rig.state.ensure_sensor(2, 2)) == pres.status_label(NodeStatus.UNKNOWN)

    run(scenario())


def test_unbound_removes_an_absent_sensor_and_the_count_follows() -> None:
    async def scenario() -> None:
        rig = Rig()
        await rig.feed(assoc(2, 1, UUID_B, "LOST", -90))
        assert rig.condition().sensors_absent == 1
        await rig.feed(assoc(2, 1, UUID_B, "UNBOUND"))
        assert rig.condition().sensors_absent == 0

    run(scenario())


@pytest.fixture(autouse=True)
def in_tmp_dir(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)


async def test_the_status_band_and_home_open_clean_with_absent_sensors(tmp_path, prefs_file):
    app = PicoTuiApp(
        enable_file_log=False, security_mode="off", preferences_path=prefs_file, operator_mode="off",
    )
    async with app.run_test(size=(150, 45)) as pilot:
        await settle(pilot)
        app.state_store.set_connection(ConnectionState.CONNECTED, port="COM3", mode=ConnectionMode.GATEWAY_CAN)
        for line in (
            assoc(1, 1, UUID_A, "ONLINE"),
            assoc(2, 1, UUID_B, "LOST", -90),
            assoc(2, 2, UUID_C, "STALE", -80),
        ):
            await app.decoder.decode(line)
        await settle(pilot)

        band = app.screen.query_one("#status-band").source_text()
        assert "Operação normal" in band
        assert "1 de 1 sensores normais" in band
        assert "2 sensores pré-aprovados ausentes" in band
        assert "crítico" not in band.lower() and "atenção" not in band.lower()
        # A seção "Precisa de atenção" nem aparece.
        assert app.screen.query_one("#sec-attention").display is False
