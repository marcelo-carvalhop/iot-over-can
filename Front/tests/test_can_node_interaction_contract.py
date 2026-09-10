import asyncio

from pico_tui.core.event_bus import EventBus
from pico_tui.core.state_store import StateStore
from pico_tui.protocol.legacy_gateway import LegacyGatewayDecoder
from pico_tui.services.controller import DomainController


def test_local_sensor_stream_updates_physical_node_live_state():
    async def scenario():
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        decoder = LegacyGatewayDecoder(bus)

        await decoder.decode("[NODE 4] [SENSOR TX] sensor=4 rodada=10 valor=0xAA")
        first = state.find_node(4)
        assert first is not None
        assert first.local_sensor_value == 0xAA
        assert first.local_sensor_last_round == 10
        assert first.rx_count == 1
        assert first.sensors == {}

        await decoder.decode("[NODE 4] [SENSOR TX] sensor=4 rodada=11 valor=0xAA")
        second = state.find_node(4)
        assert second is not None
        assert second.local_sensor_last_round == 11
        assert second.rx_count == 2

    asyncio.run(scenario())


def test_node_click_opens_live_node_detail_contract():
    from pathlib import Path
    source = (Path(__file__).resolve().parents[1] / "pico_tui" / "app.py").read_text()
    assert "self.run_worker(self._open_can_node_detail(node_id))" in source
    assert "CanNodeDetailScreen(node_id, self.state_store.find_node)" in source


def test_node_dtc_is_independent_from_wireless_sensor_dtc():
    from pico_tui.core.models import DtcRecord, Severity

    state = StateStore()
    state.update_node(3, status=__import__(
        "pico_tui.core.models", fromlist=["NodeStatus"]
    ).NodeStatus.ONLINE)
    state.add_node_dtc(3, DtcRecord(code=0x4002, severity=Severity.WARNING))
    node = state.find_node(3)
    assert node is not None
    assert node.active_dtc_count == 1
    assert node.sensors == {}
