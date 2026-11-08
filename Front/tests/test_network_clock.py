"""Relógio da rede: a TUI leva a hora do computador à Probe 00 e mostra os ajustes."""

import asyncio
import time

from pico_tui.app import PicoTuiApp
from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import GatewayDetected, LogEvent
from pico_tui.protocol.gateway_text import GatewayTextDecoder
from pico_tui.protocol.legacy_gateway import LegacyGatewayDecoder


def _collect(bus: EventBus, kind):
    seen = []

    async def handler(event):
        seen.append(event)

    bus.subscribe(kind, handler)
    return seen


def test_probe_version_carries_features():
    async def scenario():
        bus = EventBus()
        seen = _collect(bus, GatewayDetected)
        await GatewayTextDecoder(bus).decode(
            "PROBE_VERSION FIRMWARE=0.21.0 PROTOCOL=CAN_CLASSIC_V1 NODE=0 FEATURES=BUS_METRICS,TIME_SYNC"
        )
        assert seen and "TIME_SYNC" in seen[0].features.split(",")

    asyncio.run(scenario())


def test_clock_lines_become_readable_log_entries():
    async def scenario():
        bus = EventBus()
        logs = _collect(bus, LogEvent)
        decoder = LegacyGatewayDecoder(bus)
        assert await decoder.decode("[GW] TIME_SYNC node=0 fonte=TERMINAL epoch=1793003613.505 ajuste_ms=NA")
        assert await decoder.decode("[GW] TIME_SYNC node=0 fonte=TERMINAL epoch=1793004213.505 ajuste_ms=12")
        assert await decoder.decode("[GW] TIME_SYNC node=3 fonte=RTC epoch=1793004213.505 ajuste_ms=-4500")
        assert await decoder.decode("[GW] TIME_REQ node=2")
        clock = [e for e in logs if e.source == "RELOGIO"]
        assert [e.level for e in clock] == ["INFO", "DEBUG", "INFO", "DEBUG"]
        assert clock[0].message == "Relógio da rede: hora do terminal, pela Probe 00"
        assert clock[2].message == "Relógio da rede: hora de um relógio de tempo real, pelo Node 03 (ajuste de -4500 ms)"
        assert clock[3].message == "Node 02 pediu a hora da rede"

    asyncio.run(scenario())


def test_tui_sends_computer_time_only_to_a_probe_that_supports_it():
    app = PicoTuiApp(enable_file_log=False, security_mode="off")
    sent: list[str] = []
    app._send_raw = lambda command: sent.append(command) or True  # type: ignore[method-assign]
    app.serial_client = object()  # type: ignore[assignment]

    asyncio.run(app._on_gateway_detected(GatewayDetected("0.20.0", "CAN_CLASSIC_V1", "BUS_METRICS")))
    assert sent == []

    before = time.time_ns() // 1_000_000
    asyncio.run(app._on_gateway_detected(GatewayDetected("0.21.0", "CAN_CLASSIC_V1", "BUS_METRICS,TIME_SYNC")))
    after = time.time_ns() // 1_000_000
    assert len(sent) == 1 and sent[0].startswith("TIME ")
    assert before <= int(sent[0].split()[1]) <= after

    app._sync_network_clock()  # ressincronização periódica
    assert len(sent) == 2
    app.serial_client = None
    app._sync_network_clock()
    assert len(sent) == 2
