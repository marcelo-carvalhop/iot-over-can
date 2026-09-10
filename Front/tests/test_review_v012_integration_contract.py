from __future__ import annotations

import asyncio
from pathlib import Path

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import CommandAck
from pico_tui.protocol.legacy_gateway import LegacyGatewayDecoder


ROOT = Path(__file__).resolve().parents[2]


def test_can_sensor_command_ack_is_reported_to_tui() -> None:
    async def scenario() -> None:
        bus = EventBus()
        decoder = LegacyGatewayDecoder(bus)
        events: list[CommandAck] = []
        bus.subscribe(CommandAck, events.append)

        parsed = await decoder.decode(
            "[GW] CMD_ACK node=4 subcmd=0x10 action=0x11 result=APPLIED"
        )
        assert parsed is True
        assert len(events) == 1
        assert events[0].state == "APPLIED"
        assert events[0].payload["NODE"] == 4
        assert events[0].payload["ACTION"] == 0x11

    asyncio.run(scenario())


def test_can_firmware_emits_explicit_ack_for_directed_sensor_actions() -> None:
    source = (
        ROOT
        / "Codigo"
        / "node-can"
        / "src"
        / "comandos.cpp"
    ).read_text()
    protocol = (
        ROOT
        / "Codigo"
        / "node-can"
        / "include"
        / "protocolo.h"
    ).read_text()
    assert "CTRL_SUBCMD_COMMAND_ACK" in protocol
    assert "sendCommandAck(CTRL_SUBCMD_SENSOR, action, CTRL_ACK_APPLIED)" in source
    assert "sendCommandAck(CTRL_SUBCMD_SENSOR, action, CTRL_ACK_REJECTED)" in source


def test_tui_waits_for_device_auth_ack_before_mutation() -> None:
    source = (ROOT / "Front" / "pico_tui" / "app.py").read_text()
    assert "self._pending_device_command = command" in source
    assert 'client.write_line(f"AUTH UNLOCK {token}")' in source
    assert 'if "AUTH_UNLOCKED" in command_upper' in source
    assert "if pending:" in source
    assert "self._send_raw(pending)" in source


def test_wifi_off_keeps_cyw43_available_for_future_ble() -> None:
    source = (
        ROOT / "Codigo" / "node-wifi" / "edge_network_driver.c"
    ).read_text()
    assert "cyw43_arch_disable_sta_mode();" in source
    assert "cyw43_arch_deinit();" not in source
