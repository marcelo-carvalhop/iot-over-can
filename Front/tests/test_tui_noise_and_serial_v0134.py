import asyncio
from pathlib import Path

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import LogEvent, PhysicalNodeReceived
from pico_tui.core.models import NodeStatus
from pico_tui.core.state_store import StateStore
from pico_tui.protocol.legacy_gateway import LegacyGatewayDecoder
from pico_tui.services.controller import DomainController

ROOT = Path(__file__).resolve().parents[2]
UUID = "0xE6616408432B6F39"


def test_control_rx_state_update_is_consumed_without_unparsed_noise():
    async def scenario():
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        decoder = LegacyGatewayDecoder(bus)
        logs = []
        bus.subscribe(LogEvent, lambda event: logs.append(event))

        ok = await decoder.decode("[GW] CONTROLE RX 23 50 4 0")
        assert ok
        node = state.find_node(4)
        assert node is not None
        assert node.status == NodeStatus.ONLINE
        assert node.role == "LEADER"
        assert not [e for e in logs if "GW_UNPARSED" in e.message]
        assert [e for e in logs if e.source == "CAN_MAINT"]

    asyncio.run(scenario())


def test_routine_status_request_is_silent():
    async def scenario():
        bus = EventBus()
        decoder = LegacyGatewayDecoder(bus)
        logs = []
        bus.subscribe(LogEvent, lambda event: logs.append(event))
        ok = await decoder.decode("[STATUS TX] Requisicao global de status enviada")
        assert ok
        assert len(logs) == 1
        assert logs[0].source == "CAN_MAINT"

    asyncio.run(scenario())


def test_local_demo_telemetry_updates_state_without_eventlog_line():
    async def scenario():
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        decoder = LegacyGatewayDecoder(bus)
        logs = []
        bus.subscribe(LogEvent, lambda event: logs.append(event))

        ok = await decoder.decode("[GW] SENSOR sensor=2 rodada=33 valor=0xAA ativo=1")
        assert ok
        node = state.find_node(2)
        assert node is not None
        assert node.local_sensor_value == 0xAA
        assert node.local_sensor_last_round == 33
        assert not [e for e in logs if "LOCAL_SENSOR_DEMO_VALUE" in e.message]

    asyncio.run(scenario())


def test_ble_logs_only_first_observation_per_reporter_and_uuid():
    async def scenario():
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        decoder = LegacyGatewayDecoder(bus)
        logs = []
        bus.subscribe(LogEvent, lambda event: logs.append(event))

        for rssi in (-60, -55, -52):
            await decoder.decode(
                f"[GW] WIRELESS_CANDIDATE reporter=1 uuid={UUID} "
                f"profile=VIBRATION rssi={rssi} protocol=5"
            )
        ble_logs = [e for e in logs if e.source == "BLE"]
        assert len(ble_logs) == 1
        node = state.find_node(1)
        assert node is not None
        assert node.wireless_candidates[UUID].rssi_dbm == -52
        assert node.wireless_discovery_state == "SCANNING"

    asyncio.run(scenario())


def test_ble_capability_marks_functional_node_as_scanning_before_first_candidate():
    async def scenario():
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        await bus.publish(
            PhysicalNodeReceived(
                3,
                {"STATE": "ONLINE", "CAPS": "CAN,LOCAL_SENSOR,BLE_SCAN", "LOCAL_PROFILE": "DEMO_BYTE"},
            )
        )
        node = state.find_node(3)
        assert node is not None
        assert node.wireless_discovery_state == "SCANNING"

    asyncio.run(scenario())


def test_main_log_filter_and_structured_telemetry_logs_are_wired():
    app = (ROOT / "Front" / "pico_tui" / "app.py").read_text()
    assert "_event_is_main_log_noise" in app
    log_manager = (ROOT / "Front" / "pico_tui" / "services" / "log_manager.py").read_text()
    assert '"type": "local_node_telemetry"' in log_manager
    assert '"type": "wireless_candidate"' in log_manager


def test_noise_filter_hides_local_demo_values_but_keeps_errors():
    from pico_tui.app import PicoTuiApp

    assert PicoTuiApp._event_is_main_log_noise(LogEvent("DEBUG", "x LOCAL_SENSOR_DEMO_VALUE=0xAA", "CAN_NODE"))
    assert PicoTuiApp._event_is_main_log_noise(LogEvent("DEBUG", "anything", "CAN_MAINT"))
    assert not PicoTuiApp._event_is_main_log_noise(LogEvent("ERROR", "CAN_MAINT failure", "CAN_MAINT"))


def test_serial_boot_sanitization_and_serialized_probe_contract():
    serial_client = (ROOT / "Front" / "pico_tui" / "serial_client.py").read_text()
    app = (ROOT / "Front" / "pico_tui" / "app.py").read_text()
    assert "reset_input_buffer()" in serial_client
    assert 'chunk.replace(b"\\x00", b"")' in serial_client
    assert "await asyncio.sleep(0.85)" in app
    assert "client.discard_input" in app
    assert 'self._send_raw("VERSION")' in app
    assert 'for command in ("VERSION", "STATUS", "PROBE_VERSION", "PROBE_STATUS")' not in app
