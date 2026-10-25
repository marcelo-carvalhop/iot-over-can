"""Painel em papel eletrônico (perfil PANEL) visto pela TUI.

A gravação ``panel_papercolor.log`` é a saída da Probe 00 na bancada virtual,
com o firmware do PaperColor (o mesmo PanelApp do aparelho) associado ao Node 2.
"""
from __future__ import annotations

import asyncio
from pathlib import Path

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import LogEvent, PanelReported
from pico_tui.core.state_store import StateStore
from pico_tui.history.service import HistoryService
from pico_tui.history.storage import HistoryStore
from pico_tui.presentation import DATA_LINK_LABELS
from pico_tui.protocol.router import DecoderRouter
from pico_tui.services.controller import DomainController

REPLAY = Path(__file__).resolve().parents[1] / "replays" / "panel_papercolor.log"


def _run(lines: list[str], tmp_path: Path):
    bus = EventBus()
    state = StateStore()
    DomainController(bus, state)
    store = HistoryStore(tmp_path / "history.sqlite3")
    history = HistoryService(bus, store, "painel", origin="TEST")
    router = DecoderRouter(bus, "auto")
    panels: list[PanelReported] = []
    logs: list[LogEvent] = []

    async def on_panel(event: PanelReported) -> None:
        panels.append(event)

    async def on_log(event: LogEvent) -> None:
        logs.append(event)

    bus.subscribe(PanelReported, on_panel)
    bus.subscribe(LogEvent, on_log)

    async def feed() -> None:
        for line in lines:
            await router.decode(line)

    asyncio.run(feed())
    history.close()
    return state, store, panels, logs


def _probe_lines() -> list[str]:
    return [raw.split(" ", 1)[1] for raw in REPLAY.read_text(encoding="utf-8").splitlines() if raw.startswith("@")]


def test_panel_lines_are_recognized_as_probe_protocol() -> None:
    bus = EventBus()
    router = DecoderRouter(bus, "auto")
    asyncio.run(router.decode("PANEL NODE=2 CHILD=1 TYPE=STATUS STATE=AWAKE SUPPLY=USB SLEEP_S=0 LEVEL=0"))
    assert router.mode.name == "GATEWAY_CAN"


def test_operator_marks_reach_history_and_sensor_incidents(tmp_path: Path) -> None:
    state, store, panels, logs = _run(_probe_lines(), tmp_path)
    marks = [p for p in panels if p.kind == "MARK"]
    assert [m.payload["KIND"] for m in marks[:2]] == ["NOTE", "BUTTON"]
    assert marks[0].text == "troquei o cabo" and marks[0].parent_node_id == 2 and marks[0].child_id == 1
    events = store.list_events("painel")
    operator = [e for e in events if e.category == "OPERATOR"]
    assert operator[0].kind == "OPERATOR_NOTE" and "troquei o cabo" in operator[0].title
    assert operator[1].kind == "OPERATOR_BUTTON"
    assert any(log.source == "PAINEL" and "troquei o cabo" in log.message for log in logs)
    sensor = state.snapshot().nodes[2].sensors[1]
    assert any(i.kind == "OPERADOR" and "troquei o cabo" in i.message for i in sensor.incidents)


def test_sleep_cycle_is_not_a_link_incident(tmp_path: Path) -> None:
    _state, store, panels, logs = _run(_probe_lines(), tmp_path)
    statuses = [p.payload.get("STATE") for p in panels if p.kind == "STATUS"]
    assert "AWAKE" in statuses and "SLEEP" in statuses
    # O ciclo SECURE <-> SLEEPING do painel não gera aviso nem evento de enlace.
    assert not any(log.level == "WARNING" and "02.01" in log.message for log in logs)
    link_events = [e for e in store.list_events("painel") if e.kind == "LINK_STATE" and e.sensor_id == "02.01"]
    assert all("SLEEPING" not in e.title for e in link_events)
    assert "SLEEPING" in DATA_LINK_LABELS


def test_panel_status_carries_environment_and_page() -> None:
    line = next(raw for raw in _probe_lines() if raw.startswith("PANEL ") and "TYPE=STATUS" in raw)
    for key in ("BAT_PCT=80", "TEMP_C=23.5", "RH_PCT=48.0", "KEYBOARD=YES", "CLOCK=YES", "PAGE=0x"):
        assert key in line
