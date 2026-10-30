"""Etapas da criação do canal de dados (v0.22): relatos do Node, tela do sensor e tela inicial."""
from __future__ import annotations

import asyncio
import time

from pico_tui import data_channel
from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import LogEvent, WirelessEventReceived
from pico_tui.core.models import DataLinkEvent, PhysicalNode, SensorNode
from pico_tui.core.state_store import StateStore
from pico_tui.protocol.router import DecoderRouter
from pico_tui.services.controller import DomainController
from pico_tui.widgets import channel_stage_items, node_prompt, sensor_prompt

UUID = "0xE6616408432B6F39"
ASSOC = f"[GW] WIRELESS_ASSOC node=4 child=1 uuid={UUID} profile=VIBRATION state=BOUND rssi=-50 protocol=6"


def _event(line: str) -> str:
    return f"[GW] WIRELESS_EVENT node=4 child=1 {line}"


def _domain() -> tuple[StateStore, DecoderRouter, list[LogEvent], list[WirelessEventReceived]]:
    bus = EventBus()
    state = StateStore()
    DomainController(bus, state)
    router = DecoderRouter(bus, "gateway")
    logs: list[LogEvent] = []
    events: list[WirelessEventReceived] = []

    async def log(event: LogEvent) -> None:
        logs.append(event)

    async def wireless(event: WirelessEventReceived) -> None:
        events.append(event)

    bus.subscribe(LogEvent, log)
    bus.subscribe(WirelessEventReceived, wireless)
    return state, router, logs, events


def _stages(view: data_channel.ChannelView) -> dict[str, str]:
    return {stage.key: stage.state for stage in view.stages}


def _sensor(*events: DataLinkEvent, link: str = "NONE", profile: str = "VIBRATION") -> SensorNode:
    sensor = SensorNode(4, 1, wireless_uuid=UUID, profile_id=profile, association_state="BOUND")
    sensor.data_link_state = link
    sensor.data_link_bound_wall = 1000.0
    for event in events:
        sensor.data_link_events.append(event)
    return sensor


def _ev(event: str, at: float, reason: str = "NONE", **kw) -> DataLinkEvent:
    return DataLinkEvent(event=event, reason=reason, received_wall_time=at, since_s=int(at - 1000), parent_node_id=4, **kw)


def test_probe_line_becomes_event_with_reason_and_uuid() -> None:
    async def scenario() -> None:
        state, router, logs, events = _domain()
        await router.decode(ASSOC)
        await router.decode(_event("event=HELLO_REJECT reason=KEY count=3 value=7 stations=1 since_s=42"))
        await router.decode(
            "[GW] WIRELESS_EVENT node=2 child=0 event=HELLO_REJECT reason=NOT_BOUND count=1 value=9 stations=1 "
            f"since_s=5 uuid={UUID}"
        )
        assert [(e.parent_node_id, e.child_id, e.event, e.reason, e.count) for e in events] == [
            (4, 1, "HELLO_REJECT", "KEY", 3),
            (2, 0, "HELLO_REJECT", "NOT_BOUND", 1),
        ]
        assert events[1].wireless_uuid == UUID
        sensor = state.snapshot().nodes[4].sensors[1]
        # O HELLO enviado ao Módulo 02 aparece na tela do sensor, que é do Módulo 04.
        assert [(e.event, e.reason, e.parent_node_id) for e in sensor.data_link_events] == [
            ("HELLO_REJECT", "KEY", 4),
            ("HELLO_REJECT", "NOT_BOUND", 2),
        ]
        link_logs = [log.message for log in logs if log.source == "LINK"]
        assert "04.01: HELLO recusado: a chave do sensor não confere com a do módulo (3 vezes)" in link_logs
        assert any("Módulo 02: HELLO de " + UUID in text and "04.01" in text for text in link_logs)
        view = data_channel.channel_view(sensor)
        assert view.current is not None and view.current.key == "hello"
        assert "Módulo 02" in view.hint and "Desassocie" in view.hint

    asyncio.run(scenario())


def test_event_for_unknown_child_does_not_create_a_sensor() -> None:
    async def scenario() -> None:
        state, router, logs, _events = _domain()
        await router.decode(_event("event=AP_UP reason=NONE count=1 value=6 stations=0 since_s=0"))
        node = state.snapshot().nodes.get(4)
        assert node is None or 1 not in node.sensors
        assert not [log for log in logs if log.source == "LINK"]

    asyncio.run(scenario())


def test_module_facts_are_kept_on_the_module_and_shown_with_waiting_sensors() -> None:
    async def scenario() -> None:
        state, router, logs, _events = _domain()
        await router.decode(ASSOC)
        for line in (
            "event=AP_UP reason=NONE count=1 value=6 stations=0 since_s=0",
            "event=OFFER reason=NONE count=1 value=6 stations=0 since_s=0",
        ):
            await router.decode(_event(line))
        await router.decode("[GW] WIRELESS_EVENT node=4 child=0 event=STATION_JOIN reason=NONE count=1 value=0 stations=1 since_s=3")
        await router.decode("[GW] WIRELESS_EVENT node=4 child=0 event=STATION_IP reason=NONE count=1 value=2 stations=1 since_s=3")
        snapshot = state.snapshot()
        node = snapshot.nodes[4]
        sensor = node.sensors[1]
        assert [e.event for e in node.data_link_events] == ["STATION_JOIN", "STATION_IP"]
        assert [e.event for e in sensor.data_link_events] == ["AP_UP", "OFFER"]
        view = data_channel.channel_view(sensor, node.data_link_events)
        assert _stages(view) == {
            "bind": "done", "ap": "done", "offer": "done", "station": "done",
            "hello": "current", "session": "pending", "telemetry": "pending",
        }
        assert "Módulo 04: a estação recebeu endereço IP (final .2)" in [log.message for log in logs]
        # O instante do vínculo vem do tempo contado pelo módulo.
        assert sensor.data_link_bound_wall > 0

    asyncio.run(scenario())


def test_waiting_for_the_sensor_to_join_explains_what_to_check() -> None:
    sensor = _sensor(_ev("AP_UP", 1000, value=6), _ev("OFFER", 1001, value=6), link="OFFERING")
    view = data_channel.channel_view(sensor, now=1131)
    assert _stages(view)["station"] == "current"
    assert view.waiting_s == 130
    assert "IOC-04" in view.hint and "ao alcance" in view.hint
    lines = [item.plain for item in channel_stage_items(view)]
    assert lines[2].endswith("canal 6")
    assert "aguardando há 2 min 10 s" in lines[3]
    assert data_channel.waiting_summary(sensor, now=1131) == (
        "canal de dados: aguardando o sensor entrar na rede Wi-Fi do módulo, há 2 min 10 s"
    )


def test_rejected_key_and_unanswered_challenge_are_failures_with_hints() -> None:
    key = _sensor(_ev("AP_UP", 1000), _ev("OFFER", 1000), _ev("HELLO_REJECT", 1004, "KEY", count=3))
    view = data_channel.channel_view(key, now=1010)
    assert _stages(view)["station"] == "done"          # quem manda HELLO já está na rede
    assert _stages(view)["hello"] == "failed"
    assert "Reprovisione o sensor" in view.hint
    assert "(3 vezes)" in view.current.detail

    timeout = _sensor(_ev("AP_UP", 1000), _ev("OFFER", 1000), _ev("HELLO_OK", 1003), _ev("HANDSHAKE_TIMEOUT", 1008))
    view = data_channel.channel_view(timeout, now=1010)
    assert _stages(view)["hello"] == "done" and _stages(view)["session"] == "failed"
    assert "CHALLENGE" in view.hint and "antena" in view.hint
    # Um HELLO conferido depois recomeça a espera pela resposta.
    timeout.data_link_events.append(_ev("HELLO_OK", 1012))
    assert _stages(data_channel.channel_view(timeout, now=1013))["session"] == "current"


def test_session_drop_restarts_from_the_offer_and_tells_why() -> None:
    sensor = _sensor(
        _ev("AP_UP", 1000), _ev("OFFER", 1000), _ev("HELLO_OK", 1002), _ev("SESSION_UP", 1002),
        _ev("SESSION_DOWN", 1100, "TIMEOUT"),
    )
    view = data_channel.channel_view(sensor, now=1105)
    assert _stages(view) == {
        "bind": "done", "ap": "done", "offer": "current", "station": "pending",
        "hello": "pending", "session": "pending", "telemetry": "pending",
    }
    assert "o sensor parou de enviar dados" in view.previous_drop


def test_access_point_outage_is_not_hidden_by_the_link_state() -> None:
    sensor = _sensor(_ev("AP_UP", 1000), _ev("OFFER", 1000), _ev("AP_DOWN", 1050, "TEST"), link="OFFERING")
    sensor.data_link_state_wall = 1060.0   # o Node continua informando OFFERING durante a queda
    view = data_channel.channel_view(sensor, now=1070)
    assert _stages(view)["ap"] == "failed"
    assert "ensaio de falha" in view.current.detail


def test_old_firmware_without_events_falls_back_to_the_link_state() -> None:
    offering = _sensor(link="OFFERING")
    assert _stages(data_channel.channel_view(offering, now=1010))["station"] == "current"
    handshake = _sensor(link="HANDSHAKE")
    assert _stages(data_channel.channel_view(handshake, now=1010))["session"] == "current"
    no_key = _sensor(link="NO_KEY")
    view = data_channel.channel_view(no_key, now=1010)
    assert _stages(view)["ap"] == "failed" and "chave de enlace" in view.hint
    secure = _sensor(link="SECURE")
    assert _stages(data_channel.channel_view(secure, now=1010))["telemetry"] == "current"


def test_panel_has_no_telemetry_stage() -> None:
    panel = _sensor(link="SECURE", profile="PANEL")
    view = data_channel.channel_view(panel, now=1010)
    assert "telemetry" not in _stages(view) and view.complete
    assert data_channel.waiting_summary(panel, now=1010) == ""


def test_home_lines_show_the_stage_and_drop_the_sensor_count() -> None:
    sensor = _sensor(_ev("AP_UP", time.time()), _ev("OFFER", time.time()), link="OFFERING")
    text = sensor_prompt(sensor, 120).plain
    assert "canal de dados: aguardando o sensor entrar na rede Wi-Fi do módulo" in text
    assert "canal de dados" not in sensor_prompt(sensor, 120, gateway=False).plain

    node = PhysicalNode(4, role="LEADER")
    node.last_seen_monotonic = time.monotonic()
    node.sensors[1] = sensor
    prompt = node_prompt(node, 120).plain
    assert "sensor" not in prompt and "último contato" not in prompt
    assert "\n" not in prompt   # em operação normal o módulo ocupa uma linha


def test_benign_rejections_go_to_debug() -> None:
    assert data_channel.event_level(DataLinkEvent("HELLO_REJECT", "REPLAY")) == "DEBUG"
    assert data_channel.event_level(DataLinkEvent("HELLO_REJECT", "KEY")) == "WARNING"
    assert data_channel.event_level(DataLinkEvent("SESSION_UP")) == "INFO"
    assert data_channel.event_text(DataLinkEvent("STATION_JOIN", stations=2)) == (
        "uma estação entrou no ponto de acesso (2 no total)"
    )
