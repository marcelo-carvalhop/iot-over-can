from pathlib import Path
import asyncio

from pico_tui.core.event_bus import EventBus
from pico_tui.core.state_store import StateStore
from pico_tui.protocol.legacy_gateway import LegacyGatewayDecoder
from pico_tui.services.controller import DomainController

ROOT = Path(__file__).resolve().parents[2]


def test_pico_ble_beacon_is_enabled_in_build_contract():
    cmake = (ROOT / "Codigo/node-wifi/CMakeLists.txt").read_text()
    beacon = (ROOT / "Codigo/node-wifi/edge_ble_beacon.c").read_text()
    assert "pico_btstack_ble" in cmake
    assert "pico_btstack_cyw43" in cmake
    assert "BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA" in beacon
    assert "edge_device_uuid64" in beacon


def test_can_nodes_use_compact_two_frame_wireless_report():
    ids = (ROOT / "Codigo/node-can/include/can_ids.h").read_text()
    discovery = (ROOT / "Codigo/node-can/src/wireless_discovery.cpp").read_text()
    assert "CAN_ID_WIRELESS_DISCOVERY_BASE" in ids
    assert "baseId + 1u" in discovery
    assert "a.len = 8" in discovery
    assert "b.len = 8" in discovery
    assert "NODE_ID == 0" in discovery


def test_build_pico_defaults_to_actual_pico_w():
    script = (ROOT / "scripts/build_pico.sh").read_text()
    cmake = (ROOT / "Codigo/node-wifi/CMakeLists.txt").read_text()
    assert 'PICO_BOARD_TARGET:-pico_w' in script
    assert 'set(PICO_BOARD pico_w' in cmake


def test_gateway_candidate_line_updates_reporting_node_state():
    async def scenario():
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        decoder = LegacyGatewayDecoder(bus)
        ok = await decoder.decode(
            "[GW] WIRELESS_CANDIDATE reporter=2 uuid=0xE6616408432B6F39 "
            "profile=VIBRATION rssi=-48 protocol=5"
        )
        assert ok
        snap = state.snapshot()
        node = snap.nodes[2]
        assert node.wireless_candidate_count == 1
        assert node.wireless_discovery_state == "SCANNING"
        candidate = node.wireless_candidates["0xE6616408432B6F39"]
        assert candidate.rssi_dbm == -48
        assert candidate.profile_id == "VIBRATION"
        assert "BLE_SCAN" in node.capabilities

    asyncio.run(scenario())
