"""Fluxos ponta a ponta da TUI (modo demonstração e firmware serial simulado)."""
from __future__ import annotations

import json

import pytest

from pico_tui.app import PicoTuiApp
from pico_tui.core.models import ConnectionMode, ConnectionState
from pico_tui.dialogs import ConfigScreen, ConfirmScreen, ParameterScreen
from pico_tui.screens import (
    CommandScreen,
    FftScreen,
    HelpScreen,
    HomeScreen,
    MessagesScreen,
    NetworkScreen,
    NodeScreen,
    SensorScreen,
    WirelessScreen,
)
from tests.conftest import settle, wait_for


def demo_app(prefs_file) -> PicoTuiApp:
    return PicoTuiApp(demo=True, enable_file_log=False, security_mode="off", preferences_path=prefs_file)


def direct_app(fake_fw, security_file, prefs_file) -> PicoTuiApp:
    return PicoTuiApp(
        port=fake_fw.slave_name,
        mode="sensor",
        enable_file_log=False,
        security_mode="off",
        security_config=str(security_file),
        preferences_path=prefs_file,
    )


async def test_demo_opens_on_home_with_equipment_list(prefs_file) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot, 8)
        state = app.state_store.snapshot()
        assert isinstance(app.screen, HomeScreen)
        assert state.connection_state == ConnectionState.READY
        assert state.connection_mode == ConnectionMode.DEMO
        assert state.selected_logical_id == "20.01"  # seleção implícita do primeiro sensor
        ids = app.screen.query_one("#equipment-list")._ids
        assert ids[:3] == ("node:20", "sensor:20.01", "sensor:20.02")
        assert sum(1 for item in ids if item.startswith("sensor:")) == 3
        band = app.screen.query_one("#status-band")
        assert band.has_class("band-normal")
        assert "Operação normal" in band.source_text()


async def test_enter_opens_module_then_sensor_and_esc_returns(prefs_file) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot, 6)
        await pilot.press("enter")
        await settle(pilot, 2)
        assert isinstance(app.screen, NodeScreen) and app.screen.node_id == 20
        await pilot.press("escape")
        await settle(pilot, 2)
        await pilot.press("down", "enter")
        await settle(pilot, 3)
        assert isinstance(app.screen, SensorScreen) and app.screen.logical_id == "20.01"
        metrics = app.screen.query_one("#sensor-metrics").source_text()
        assert "RMS" in metrics and "Frequência dominante" in metrics
        assert app.screen.query_one("#sec-metrics").display is True
        await pilot.press("escape")
        await settle(pilot, 2)
        assert isinstance(app.screen, HomeScreen)


async def test_global_letter_shortcuts_navigate(prefs_file) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(100, 30)) as pilot:
        await settle(pilot, 5)
        for key, screen_type in (("r", NetworkScreen), ("m", MessagesScreen), ("w", WirelessScreen), ("c", CommandScreen), ("question_mark", HelpScreen)):
            await pilot.press(key)
            await settle(pilot, 2)
            assert isinstance(app.screen, screen_type), key
            await pilot.press("i")
            await settle(pilot, 2)
            assert isinstance(app.screen, HomeScreen)
        for key, screen_type in (("f5", NetworkScreen), ("f6", MessagesScreen), ("f7", WirelessScreen), ("f4", CommandScreen), ("f1", HelpScreen)):
            await pilot.press(key)
            await settle(pilot, 2)
            assert isinstance(app.screen, screen_type), key
            await pilot.press("f2")
            await settle(pilot, 2)


async def test_sensor_screen_c_opens_commands_targeting_sensor(prefs_file) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(120, 40)) as pilot:
        await settle(pilot, 5)
        app.open_target("sensor:20.02")
        await settle(pilot, 3)
        await pilot.press("c")
        await settle(pilot, 3)
        assert isinstance(app.screen, CommandScreen)
        assert app.screen.target == "sensor:20.02"
        ids = app.screen.query_one("#cmd-actions")._ids
        assert "sensor.fft" in ids and "sensor.configure" in ids
        assert "network.election" not in ids


async def test_election_requires_confirmation_and_sends_frame(prefs_file) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(120, 40)) as pilot:
        await settle(pilot, 5)
        await pilot.press("c")
        await settle(pilot, 3)
        actions = app.screen.query_one("#cmd-actions")
        actions.highlighted = actions._ids.index("network.election")
        await pilot.press("enter")
        await settle(pilot, 3)
        assert isinstance(app.screen, ConfirmScreen)
        await pilot.press("n")
        await settle(pilot, 2)
        assert "22 00 FF 01" not in (app.state_store.snapshot().last_action or "")
        await pilot.press("enter")
        await settle(pilot, 2)
        await pilot.press("s")
        await settle(pilot, 3)
        assert "22 00 FF 01" in app.state_store.snapshot().last_action


async def test_liveness_parameter_uses_firmware_mapping(prefs_file) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(120, 40)) as pilot:
        await settle(pilot, 5)
        app.start_action("network.liveness", "network")
        await settle(pilot, 3)
        assert isinstance(app.screen, ParameterScreen)
        choices = app.screen.query_one("#parameter-choice")
        choices.highlighted = 4  # 500 ms
        await pilot.press("enter")
        await settle(pilot, 3)
        assert "22 30 FF 05" in app.state_store.snapshot().last_action


async def test_demo_fft_opens_spectrum_screen(prefs_file) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(120, 45)) as pilot:
        await settle(pilot, 5)
        app.open_target("sensor:20.01")
        await settle(pilot, 3)
        await pilot.press("f")
        await settle(pilot, 2)
        assert isinstance(app.screen, ParameterScreen)
        await pilot.press("enter")
        await settle(pilot, 4)
        assert isinstance(app.screen, FftScreen)
        chart = str(app.screen.query_one("#fft-ascii").render())
        assert "Hz" in chart and "Magnitude" in chart


async def test_direct_serial_connects_and_populates_state(fake_fw, security_file, prefs_file) -> None:
    app = direct_app(fake_fw, security_file, prefs_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot, 12, 0.1)
        sensor = app.state_store.find_sensor("01.01")
        assert app.connected is True
        assert sensor is not None
        assert sensor.sensor_mode.value == "STRUCTURAL"
        assert sensor.configuration.sample_rate_effective_hz == 1000.0
        assert sensor.acquisition_mode.value == "POLLING"
        assert sensor.health.drdy_enabled is False
        assert app.screen.query_one("#equipment-list")._ids == ("node:01", "sensor:01.01")
        # Conexão direta não tem rede CAN: o alvo padrão dos comandos é o sensor.
        await pilot.press("c")
        await settle(pilot, 3)
        assert app.screen.target == "sensor:01.01"


async def test_mutating_command_authenticates_firmware_first(fake_fw, security_file, prefs_file) -> None:
    app = direct_app(fake_fw, security_file, prefs_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot, 12, 0.1)
        await app.execute_action("sensor.telemetry_on", "sensor:01.01")
        assert await wait_for(pilot, lambda: "TELEMETRY ON" in fake_fw.received_commands)
        assert await wait_for(pilot, lambda: app.telemetry_on is True)
        assert "AUTH UNLOCK 0x1234" in fake_fw.received_commands
        auth_index = fake_fw.received_commands.index("AUTH UNLOCK 0x1234")
        assert fake_fw.received_commands.index("TELEMETRY ON") > auth_index
        assert app.telemetry_on is True
        # O token nunca aparece no registro visível.
        assert not any("0x1234" in entry.message for entry in app.message_history)


async def test_dtc_becomes_incident_and_clear_needs_confirmation(fake_fw, security_file, prefs_file) -> None:
    app = direct_app(fake_fw, security_file, prefs_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot, 10, 0.1)
        fake_fw.inject_dtc_event(0x2002)
        band = app.screen.query_one("#status-band")
        assert await wait_for(pilot, lambda: band.has_class("band-attention"))
        sensor = app.state_store.find_sensor("01.01")
        assert sensor is not None and 0x2002 in sensor.active_dtcs
        assert any("0x2002" in incident.message for incident in sensor.incidents)
        assert any("0x2002" in line and "clipping" in line.lower() for line in app.log_messages)
        assert app.screen.query_one("#sec-attention").display is True

        app.start_action("sensor.dtc_clear", "sensor:01.01")
        await settle(pilot, 2)
        assert isinstance(app.screen, ConfirmScreen)
        await pilot.press("s")
        assert await wait_for(pilot, lambda: app.state_store.find_sensor("01.01").active_dtc_count == 0)
        assert "DTC CLEAR" in fake_fw.received_commands
        assert await wait_for(pilot, lambda: band.has_class("band-normal"))


async def test_ctrl_c_stops_direct_telemetry(fake_fw, security_file, prefs_file) -> None:
    app = direct_app(fake_fw, security_file, prefs_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot, 10, 0.1)
        # Dois comandos mutáveis seguidos: o segundo espera a autenticação, não é descartado.
        await app.execute_action("sensor.restart_acq", "sensor:01.01")
        await app.execute_action("sensor.telemetry_on", "sensor:01.01")
        assert await wait_for(pilot, lambda: app.telemetry_on is True)
        await pilot.press("ctrl+c")
        assert await wait_for(pilot, lambda: "<CTRL-C>" in fake_fw.received_commands)
        assert await wait_for(pilot, lambda: app.telemetry_on is False)
        assert "ACQ POLLING" in fake_fw.received_commands


async def test_direct_fft_flow_opens_frequency_chart(fake_fw, security_file, prefs_file) -> None:
    app = direct_app(fake_fw, security_file, prefs_file)
    async with app.run_test(size=(150, 50)) as pilot:
        await settle(pilot, 10, 0.1)
        app.start_action("sensor.fft", "sensor:01.01", "64")
        await settle(pilot, 8, 0.1)
        assert fake_fw.received_commands[-2:] == ["FFT ONCE", "TELEMETRY ONCE"]
        assert isinstance(app.screen, FftScreen)
        sensor = app.state_store.find_sensor("01.01")
        assert sensor is not None and len(sensor.latest_fft.magnitudes) == 8
        chart = str(app.screen.query_one("#fft-ascii").render())
        assert "Magnitude" in chart and "Hz" in chart


async def test_configuration_form_applies_and_waits_for_firmware(fake_fw, security_file, prefs_file) -> None:
    app = direct_app(fake_fw, security_file, prefs_file)
    async with app.run_test(size=(140, 50)) as pilot:
        await settle(pilot, 10, 0.1)
        app.start_action("sensor.configure", "sensor:01.01")
        await settle(pilot, 3)
        assert isinstance(app.screen, ConfigScreen)
        app.screen.query_one("#cfg-rate").value = "250"
        await pilot.click("#cfg-verify")
        await settle(pilot, 8, 0.1)
        assert "SET RATE 250.0" in fake_fw.received_commands
        assert "APPLY" in fake_fw.received_commands
        sensor = app.state_store.find_sensor("01.01")
        assert sensor is not None and sensor.configuration.transaction_state == "APPLIED"
        assert sensor.configuration.sample_rate_effective_hz == 250.0


async def test_manual_command_field_accepts_raw_and_internal(fake_fw, security_file, prefs_file) -> None:
    app = direct_app(fake_fw, security_file, prefs_file)
    async with app.run_test(size=(120, 40)) as pilot:
        await settle(pilot, 10, 0.1)
        await pilot.press("c")
        await settle(pilot, 2)
        await pilot.press("a")
        await settle(pilot, 2)
        # Letras digitadas no campo não disparam atalhos de navegação.
        await pilot.press(*"PING", "enter")
        await settle(pilot, 3, 0.1)
        assert isinstance(app.screen, CommandScreen)
        assert "PING" in fake_fw.received_commands
        await pilot.press(*":dtc list", "enter")
        await settle(pilot, 3, 0.1)
        assert "DTC" in fake_fw.received_commands


async def test_messages_on_home_is_a_saved_preference(prefs_file) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(120, 40)) as pilot:
        await settle(pilot, 4)
        home = app.screen
        assert home.query_one("#sec-home-messages").display is False
        await pilot.press("m")
        await settle(pilot, 2)
        await pilot.press("h")
        await settle(pilot, 2)
        assert json.loads(prefs_file.read_text())["show_messages_on_home"] is True
        await pilot.press("escape")
        await settle(pilot, 2)
        assert app.screen.query_one("#sec-home-messages").display is True


async def test_offline_start_shows_invitation_to_connect(prefs_file) -> None:
    app = PicoTuiApp(enable_file_log=False, security_mode="off", preferences_path=prefs_file)
    async with app.run_test(size=(80, 24)) as pilot:
        await settle(pilot, 3)
        assert app.state_store.snapshot().connection_state == ConnectionState.DISCONNECTED
        band = app.screen.query_one("#status-band")
        assert band.has_class("band-offline")
        assert "F3" in band.source_text()
        empty = app.screen.query_one("#equipment-list")._ids
        assert empty == ("__empty__",)


@pytest.mark.parametrize("width", [48, 80, 140])
async def test_open_node_screen_from_internal_command(prefs_file, width) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(width, 30)) as pilot:
        await settle(pilot, 5)
        await app._execute_internal_command("node 21")
        await settle(pilot, 3)
        assert isinstance(app.screen, NodeScreen) and app.screen.node_id == 21


async def test_wireless_screen_groups_same_uuid_across_nodes(prefs_file) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(120, 40)) as pilot:
        await settle(pilot, 4)
        uuid = "0xE6616408432B6F39"
        app.state_store.update_wireless_candidate(1, uuid, profile_id="VIBRATION", rssi_dbm=-71, protocol_version="5")
        app.state_store.update_wireless_candidate(4, uuid, profile_id="VIBRATION", rssi_dbm=-49, protocol_version="5")
        await pilot.press("f7")
        await settle(pilot, 2)
        assert isinstance(app.screen, WirelessScreen)
        available = app.screen.query_one("#wireless-available")
        assert available._ids == (f"candidate:{uuid.lower()}",)
        details = app.screen.query_one("#wireless-details").source_text()
        assert "Módulo 04" in details and "-49 dBm" in details
        assert "Módulo 01" in details and "-71 dBm" in details
