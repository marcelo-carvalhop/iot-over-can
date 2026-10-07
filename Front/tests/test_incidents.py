from __future__ import annotations

import asyncio
import time

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import DtcCleared, DtcReceived, TelemetryReceived
from pico_tui.core.models import DtcRecord, NodeStatus, Severity, TelemetrySample
from pico_tui.core.state_store import StateStore
from pico_tui.services.controller import DomainController


def test_repeated_incidents_are_coalesced():
    state = StateStore()
    for _ in range(5):
        state.add_sensor_incident(1, 1, Severity.WARNING, "SEQUENCIA", "Mensagens perdidas", key="sequence:lost")
    incidents = state.find_sensor("01.01").incidents
    assert len(incidents) == 1
    assert incidents[0].count == 5


def test_freshness_transitions_are_recorded_once():
    state = StateStore()
    state.update_telemetry(TelemetrySample(1, 1, rms=0.1))
    sensor = state._state.nodes[1].sensors[1]
    sensor.last_seen_monotonic = time.monotonic() - 15
    state.refresh_freshness()
    state.refresh_freshness()
    sensor.last_seen_monotonic = time.monotonic()
    state.refresh_freshness()
    messages = [incident.message for incident in state.find_sensor("01.01").incidents]
    assert messages == ["Sem dados recentes (mais de 10 s)", "Comunicação restabelecida"]
    assert state.find_sensor("01.01").status == NodeStatus.ONLINE


def test_dtc_incident_only_on_activation_and_clear():
    async def scenario():
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        for _ in range(3):
            await bus.publish(DtcReceived(1, 1, DtcRecord(0x2002, Severity.WARNING)))
        await bus.publish(DtcCleared(1, 1, None))
        return [incident.message for incident in state.find_sensor("01.01").incidents]

    messages = asyncio.run(scenario())
    assert len([message for message in messages if "0x2002" in message]) == 1
    assert messages[-1].startswith("Limpeza confirmada")


def test_clipping_and_stalta_transitions():
    async def scenario():
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        for clip in (False, True, True, False, True):
            await bus.publish(TelemetryReceived(TelemetrySample(1, 1, clipping=clip, stalta_triggered=False)))
        await bus.publish(TelemetryReceived(TelemetrySample(1, 1, clipping=False, stalta_triggered=True)))
        return state.find_sensor("01.01").incidents

    incidents = asyncio.run(scenario())
    clipping = [incident for incident in incidents if incident.key == "event:clipping"]
    assert len(clipping) == 1 and clipping[0].count == 2  # duas transições, agrupadas
    assert any(incident.key == "event:stalta" for incident in incidents)


def test_lean_snapshot_keeps_only_recent_history():
    state = StateStore()
    for sequence in range(500):
        state.update_telemetry(TelemetrySample(1, 1, sequence=sequence))
    lean = state.snapshot(history=120, frames=0)
    full = state.snapshot()
    assert len(lean.nodes[1].sensors[1].telemetry_history) == 120
    assert lean.nodes[1].sensors[1].telemetry_history[-1].sequence == 499
    assert len(full.nodes[1].sensors[1].telemetry_history) == 500
    # A cópia enxuta não altera o estado original.
    assert len(state._state.nodes[1].sensors[1].telemetry_history) == 500
