from __future__ import annotations

import asyncio
from pathlib import Path
from unittest.mock import patch

from pico_tui import commands
from pico_tui.core.event_bus import EventBus
from pico_tui.core.models import NodeStatus
from pico_tui.core.state_store import StateStore
from pico_tui.protocol.legacy_gateway import LegacyGatewayDecoder
from pico_tui.security import SecurityManager
from pico_tui.services.controller import DomainController

ROOT = Path(__file__).resolve().parents[2]
UUID = "0xE6616408432B6F39"


def test_association_protocol_contract_uses_dedicated_can_ranges() -> None:
    ids = (ROOT / "Codigo/node-can/include/can_ids.h").read_text()
    protocol = (ROOT / "Codigo/node-can/include/protocolo.h").read_text()
    source = (ROOT / "Codigo/node-can/src/wireless_discovery.cpp").read_text()

    assert "CAN_ID_WIRELESS_ASSOC_CMD_A" in ids
    assert "CAN_ID_WIRELESS_ASSOC_STATUS_BASE" in ids
    assert "WIRELESS_ASSOC_ASSOCIATING" in protocol
    assert "WIRELESS_ASSOC_ONLINE" in protocol
    assert "WIRELESS_ASSOC_STALE" in protocol
    assert "WIRELESS_ASSOC_LOST" in protocol
    assert "WIRELESS BIND" in source
    assert 'strcasecmp(verb, "UNBIND")' in source


def test_tui_builds_probe_association_commands() -> None:
    assert commands.wireless_association("BIND", 1, UUID) == f"WIRELESS BIND 1 {UUID}"
    assert commands.wireless_association("UNBIND", 4, UUID) == f"WIRELESS UNBIND 4 {UUID}"


def test_wireless_commands_are_mutating_security_operations(tmp_path: Path) -> None:
    security = SecurityManager("off", tmp_path / "missing.json")
    assert security.command_requires_auth(f"WIRELESS BIND 1 {UUID}") is True
    assert security.command_requires_auth(f"WIRELESS UNBIND 1 {UUID}") is True


def test_association_line_creates_logical_child_and_tracks_state() -> None:
    async def scenario() -> None:
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        decoder = LegacyGatewayDecoder(bus)

        assert await decoder.decode(
            f"[GW] WIRELESS_ASSOC node=1 child=1 uuid={UUID} "
            "profile=VIBRATION state=ASSOCIATING rssi=-48 protocol=5"
        )
        sensor = state.snapshot().nodes[1].sensors[1]
        assert sensor.logical_id == "01.01"
        assert sensor.wireless_uuid == UUID
        assert sensor.association_state == "ASSOCIATING"
        assert sensor.status.value == "AGING"

        assert await decoder.decode(
            f"[GW] WIRELESS_ASSOC node=1 child=1 uuid={UUID} "
            "profile=VIBRATION state=ONLINE rssi=-45 protocol=5"
        )
        sensor = state.snapshot().nodes[1].sensors[1]
        assert sensor.association_state == "ONLINE"
        assert sensor.association_rssi_dbm == -45
        assert sensor.status.value == "ONLINE"

    asyncio.run(scenario())


def test_unbound_line_removes_logical_child() -> None:
    async def scenario() -> None:
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        decoder = LegacyGatewayDecoder(bus)

        await decoder.decode(
            f"[GW] WIRELESS_ASSOC node=2 child=1 uuid={UUID} "
            "profile=VIBRATION state=ONLINE rssi=-50 protocol=5"
        )
        assert state.find_sensor("02.01") is not None

        await decoder.decode(
            f"[GW] WIRELESS_ASSOC node=2 child=1 uuid={UUID} "
            "profile=VIBRATION state=UNBOUND rssi=-50 protocol=5"
        )
        assert state.find_sensor("02.01") is None
        assert state.snapshot().selected_logical_id is None
        assert state.snapshot().selected_node_id == 2

    asyncio.run(scenario())


def test_associated_wireless_child_keeps_node_authoritative_liveness() -> None:
    state = StateStore()
    state.ensure_sensor(4, 1, wireless_uuid=UUID)
    with patch("pico_tui.core.state_store.time.monotonic", return_value=100.0):
        state.update_sensor(
            4,
            1,
            wireless_uuid=UUID,
            association_state="ONLINE",
            status=NodeStatus.ONLINE,
        )
    # Mesmo que a última mensagem de associação envelheça, a TUI não deve
    # inventar STALE/LOST. O Node CAN associado é a autoridade de liveness.
    with patch("pico_tui.core.state_store.time.monotonic", return_value=220.0):
        state.refresh_freshness(aging_after=1.0, stale_after=2.0, lost_after=3.0)
    sensor = state.find_sensor("04.01")
    assert sensor is not None
    assert sensor.status.value == "ONLINE"


def test_wireless_liveness_uses_rf_tolerant_lease_and_scan_watchdog() -> None:
    source = (ROOT / "Codigo/node-can/src/wireless_discovery.cpp").read_text()
    assert "ONLINE_STALE_MS = 15000" in source
    assert "STALE_LOST_MS = 45000" in source
    assert "SCAN_WATCHDOG_MS = 2000" in source
    assert "setDuplicateFilter(0)" in source
    assert "isScanning()" in source
    assert "BLE_SCAN_RESTART" in source
