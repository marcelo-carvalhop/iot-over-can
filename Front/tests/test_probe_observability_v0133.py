import asyncio

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import GatewayDetected, GatewayStatusReceived, LogEvent
from pico_tui.protocol.gateway_text import GatewayTextDecoder
from pico_tui.protocol.legacy_gateway import LegacyGatewayDecoder


def test_probe_version_and_status_are_visible_in_event_log():
    async def scenario():
        bus = EventBus()
        decoder = GatewayTextDecoder(bus)
        logs = []
        detected = []
        statuses = []
        bus.subscribe(LogEvent, lambda event: logs.append(event))
        bus.subscribe(GatewayDetected, lambda event: detected.append(event))
        bus.subscribe(GatewayStatusReceived, lambda event: statuses.append(event))

        await decoder.decode(
            "PROBE_VERSION FIRMWARE=0.13.2 PROTOCOL=CAN_CLASSIC_V1 NODE=0"
        )
        await decoder.decode(
            "PROBE_STATUS NODE=0 STATE=ONLINE CAN=CLASSIC ARB=500000 DATA=0 "
            "WIFI=OFF BLE_SCAN=OFF UPTIME_MS=1234"
        )

        assert detected and detected[0].firmware_version == "0.13.2"
        assert statuses and statuses[0].payload["CAN"] == "CLASSIC"
        probe_logs = [event for event in logs if event.source == "PROBE"]
        assert len(probe_logs) == 2
        assert probe_logs[0].message.startswith("PROBE_VERSION ")
        assert probe_logs[1].message.startswith("PROBE_STATUS ")

    asyncio.run(scenario())


def test_status_refresh_uses_probe_status_name():
    from pathlib import Path

    root = Path(__file__).resolve().parents[2]
    app = (root / "Front" / "pico_tui" / "app.py").read_text()
    assert 'self._send_raw("PROBE_STATUS")' in app


def test_wireless_candidate_decoder_emits_domain_event():
    from pico_tui.core.events import WirelessCandidateReceived

    async def scenario():
        bus = EventBus()
        decoder = LegacyGatewayDecoder(bus)
        candidates = []
        bus.subscribe(WirelessCandidateReceived, lambda event: candidates.append(event))

        ok = await decoder.decode(
            "[GW] WIRELESS_CANDIDATE reporter=2 uuid=0xE6616408432B6F39 "
            "profile=VIBRATION rssi=-48 protocol=5"
        )

        assert ok
        assert len(candidates) == 1
        assert candidates[0].wireless_uuid == "0xE6616408432B6F39"
        assert candidates[0].reporter_node_id == 2

    asyncio.run(scenario())
