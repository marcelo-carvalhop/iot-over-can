import asyncio

from pico_tui.core.event_bus import EventBus
from pico_tui.core.state_store import StateStore
from pico_tui.protocol.legacy_gateway import LegacyGatewayDecoder
from pico_tui.services.controller import DomainController


def test_legacy_0xaa_is_local_node_sensor_not_wireless_child():
    async def scenario():
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        decoder = LegacyGatewayDecoder(bus)

        parsed = await decoder.decode(
            "[GW] SENSOR sensor=2 rodada=7 valor=0xAA ativo=1"
        )
        assert parsed is True

        snapshot = state.snapshot()
        node = snapshot.nodes[2]
        assert node.local_sensor_profile == "DEMO_BYTE"
        assert node.local_sensor_value == 0xAA
        assert node.local_sensor_enabled is True
        assert "LOCAL_SENSOR" in node.capabilities
        assert "LOCAL_SENSOR_DEMO" in node.capabilities
        assert node.sensors == {}

    asyncio.run(scenario())


def test_probe_zero_is_not_inserted_as_functional_node():
    async def scenario():
        from pico_tui.core.events import PhysicalNodeReceived

        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)

        await bus.publish(PhysicalNodeReceived(0, {"STATE": "ONLINE"}))
        assert 0 not in state.snapshot().nodes

    asyncio.run(scenario())


def test_capability_announcement_is_parsed():
    async def scenario():
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        decoder = LegacyGatewayDecoder(bus)

        parsed = await decoder.decode(
            "[NODE 3] CAPS=CAN,LOCAL_SENSOR,LOCAL_SENSOR_DEMO LOCAL_PROFILE=DEMO_BYTE"
        )
        assert parsed is True
        node = state.snapshot().nodes[3]
        assert node.local_sensor_profile == "DEMO_BYTE"
        assert "CAN" in node.capabilities
        assert "LOCAL_SENSOR_DEMO" in node.capabilities

    asyncio.run(scenario())
