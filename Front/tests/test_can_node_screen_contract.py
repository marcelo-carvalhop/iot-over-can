"""Tela própria do módulo CAN: viva, com sensor local, descoberta e intercorrências."""
from __future__ import annotations

from pico_tui.app import PicoTuiApp
from pico_tui.core.events import LocalNodeTelemetryReceived, PhysicalNodeReceived
from pico_tui.core.models import DtcRecord, Severity
from pico_tui.screens import NodeScreen
from tests.conftest import settle


async def test_node_screen_is_live_and_shows_local_sensor(prefs_file) -> None:
    app = PicoTuiApp(enable_file_log=False, security_mode="off", preferences_path=prefs_file)
    async with app.run_test(size=(120, 40)) as pilot:
        await app.bus.publish(PhysicalNodeReceived(4, {"STATE": "ONLINE", "ROLE": "LEADER", "CAPS": "CAN,LOCAL_SENSOR,BLE_SCAN"}))
        await app.bus.publish(LocalNodeTelemetryReceived(4, 0xAA, round_number=7, enabled=True))
        app.open_target("node:04")
        await settle(pilot, 3)
        assert isinstance(app.screen, NodeScreen)
        local = app.screen.query_one("#node-local").source_text()
        assert "0xAA" in local and "Rodada 7" in local
        status = app.screen.query_one("#node-status").source_text()
        assert "Líder" in status and "Varredura BLE" in status
        await app.bus.publish(LocalNodeTelemetryReceived(4, 0xAB, round_number=8, enabled=True))
        await settle(pilot, 6)  # ciclo de atualização da interface: 0,5 s
        assert "0xAB" in app.screen.query_one("#node-local").source_text()


async def test_node_dtc_is_listed_as_node_incident(prefs_file) -> None:
    app = PicoTuiApp(enable_file_log=False, security_mode="off", preferences_path=prefs_file)
    async with app.run_test(size=(120, 40)) as pilot:
        await app.bus.publish(PhysicalNodeReceived(3, {"STATE": "ONLINE"}))
        app.state_store.add_node_dtc(3, DtcRecord(code=0x4002, severity=Severity.WARNING))
        app.open_target("node:03")
        await settle(pilot, 3)
        incidents = app.screen.query_one("#node-incidents").source_text()
        assert "0x4002" in incidents
        assert "Nenhuma intercorrência" not in incidents
