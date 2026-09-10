from __future__ import annotations

from pathlib import Path

import asyncio

import pytest

from pico_tui.app import PicoTuiApp
from pico_tui.core.models import ConnectionMode, ConnectionState
from tests.fake_firmware import FakeFirmware


@pytest.fixture
async def fake_fw():
    firmware = FakeFirmware()
    firmware.start()
    await asyncio.sleep(0.15)
    yield firmware
    firmware.stop()


@pytest.mark.asyncio
async def test_demo_starts_with_hierarchical_nodes() -> None:
    app = PicoTuiApp(demo=True, enable_file_log=False)
    async with app.run_test(size=(140, 45)) as pilot:
        for _ in range(8):
            await asyncio.sleep(0.15)
            await pilot.pause()
        state = app.state_store.snapshot()
        assert state.connection_state == ConnectionState.READY
        assert state.connection_mode == ConnectionMode.DEMO
        assert state.selected_logical_id == "20.01"
        assert sum(len(node.sensors) for node in state.nodes.values()) == 3


@pytest.mark.asyncio
async def test_direct_serial_connects_and_populates_state(fake_fw) -> None:
    app = PicoTuiApp(port=fake_fw.slave_name, mode="sensor", enable_file_log=False)
    async with app.run_test(size=(140, 45)) as pilot:
        for _ in range(14):
            await asyncio.sleep(0.1)
            await pilot.pause()
        sensor = app.state_store.find_sensor("01.01")
        assert app.connected is True
        assert sensor is not None
        assert sensor.sensor_mode.value == "STRUCTURAL"
        assert sensor.configuration.sample_rate_effective_hz == 1000.0
        assert sensor.acquisition_mode.value == "POLLING"
        assert sensor.health.drdy_irq_count == 0
        assert sensor.health.drdy_missed_count == 0
        assert sensor.health.drdy_enabled is False


@pytest.mark.asyncio
async def test_direct_keyboard_controls_update_state(fake_fw) -> None:
    app = PicoTuiApp(port=fake_fw.slave_name, mode="sensor", enable_file_log=False)
    async with app.run_test(size=(140, 45)) as pilot:
        for _ in range(12):
            await asyncio.sleep(0.1)
            await pilot.pause()
        await pilot.press("ctrl+t")
        await asyncio.sleep(0.35)
        await pilot.pause()
        assert app.telemetry_on is True
        await pilot.press("ctrl+m")
        await asyncio.sleep(0.35)
        await pilot.pause()
        assert app.simulate_on is True


@pytest.mark.asyncio
async def test_dtc_event_is_recorded(fake_fw) -> None:
    app = PicoTuiApp(port=fake_fw.slave_name, mode="sensor", enable_file_log=False)
    async with app.run_test(size=(140, 45)) as pilot:
        for _ in range(12):
            await asyncio.sleep(0.1)
            await pilot.pause()
        fake_fw.inject_dtc_event()
        await asyncio.sleep(0.3)
        await pilot.pause()
        sensor = app.state_store.find_sensor("01.01")
        assert sensor is not None
        assert 0x2002 in sensor.active_dtcs
        assert any("0x2002" in line and "Clipping" in line for line in app.log_messages)


@pytest.mark.asyncio
async def test_network_modal_opens_in_demo() -> None:
    app = PicoTuiApp(demo=True, enable_file_log=False)
    async with app.run_test(size=(140, 45)) as pilot:
        await asyncio.sleep(0.5)
        await pilot.pause()
        await pilot.press("f9")
        await pilot.pause()
        assert "NetworkScreen" in [type(screen).__name__ for screen in app.screen_stack]
        await pilot.press("escape")


@pytest.mark.asyncio
async def test_dtc_clear_works_from_internal_cli(fake_fw) -> None:
    app = PicoTuiApp(port=fake_fw.slave_name, mode="sensor", enable_file_log=False)
    async with app.run_test(size=(140, 45)) as pilot:
        for _ in range(10):
            await asyncio.sleep(0.1)
            await pilot.pause()
        fake_fw.inject_dtc_event(0x2002)
        await asyncio.sleep(0.25)
        await app._execute_internal_command("dtc clear 01.01 all")
        await asyncio.sleep(0.3)
        await pilot.pause()
        sensor = app.state_store.find_sensor("01.01")
        assert sensor is not None
        assert sensor.active_dtc_count == 0
        assert "DTC CLEAR" in fake_fw.received_commands


@pytest.mark.asyncio
async def test_restart_polling_and_stop_stream_shortcuts(fake_fw) -> None:
    app = PicoTuiApp(port=fake_fw.slave_name, mode="sensor", enable_file_log=False)
    async with app.run_test(size=(140, 45)) as pilot:
        for _ in range(10):
            await asyncio.sleep(0.1)
            await pilot.pause()
        await pilot.press("ctrl+o")
        await asyncio.sleep(0.25)
        await pilot.press("ctrl+t")
        await asyncio.sleep(0.25)
        assert app.telemetry_on is True
        await pilot.press("ctrl+c")
        await asyncio.sleep(0.3)
        await pilot.pause()
        sensor = app.state_store.find_sensor("01.01")
        assert sensor is not None
        assert sensor.acquisition_mode.value == "POLLING"
        assert "ACQ POLLING" in fake_fw.received_commands
        assert "<CTRL-C>" in fake_fw.received_commands
        assert app.telemetry_on is False


@pytest.mark.asyncio
async def test_direct_fft_once_requests_snapshot_and_receives_vector(fake_fw) -> None:
    app = PicoTuiApp(port=fake_fw.slave_name, mode="sensor", enable_file_log=False)
    async with app.run_test(size=(140, 45)) as pilot:
        for _ in range(10):
            await asyncio.sleep(0.1)
            await pilot.pause()
        await app._request_fft_for_selected(64, "VIEW_ONLY")
        await asyncio.sleep(0.35)
        await pilot.pause()
        sensor = app.state_store.find_sensor("01.01")
        assert sensor is not None
        assert sensor.latest_fft is not None
        assert len(sensor.latest_fft.magnitudes) == 8
        assert fake_fw.received_commands[-2:] == ["FFT ONCE", "TELEMETRY ONCE"]


@pytest.mark.asyncio
async def test_dtc_clear_works_from_modal_window(fake_fw) -> None:
    app = PicoTuiApp(port=fake_fw.slave_name, mode="sensor", enable_file_log=False)
    async with app.run_test(size=(140, 45)) as pilot:
        for _ in range(10):
            await asyncio.sleep(0.1)
            await pilot.pause()
        fake_fw.inject_dtc_event(0x2002)
        await asyncio.sleep(0.25)
        await pilot.press("f8")
        await pilot.pause()
        await pilot.press("a")
        await pilot.pause()
        await pilot.press("y")
        await asyncio.sleep(0.35)
        await pilot.pause()
        sensor = app.state_store.find_sensor("01.01")
        assert sensor is not None
        assert sensor.active_dtc_count == 0
        assert "DTC CLEAR" in fake_fw.received_commands

@pytest.mark.asyncio
async def test_f6_opens_live_telemetry_screen() -> None:
    app = PicoTuiApp(demo=True, enable_file_log=False)
    async with app.run_test(size=(150, 48)) as pilot:
        for _ in range(6):
            await asyncio.sleep(0.12)
            await pilot.pause()
        await pilot.press("f6")
        await pilot.pause()
        assert type(app.screen).__name__ == "TelemetryScreen"
        assert app.screen.query_one("#telemetry-live-table").row_count == 20
        await pilot.press("escape")
        await pilot.pause()
        assert type(app.screen).__name__ != "TelemetryScreen"


@pytest.mark.asyncio
async def test_f6_once_button_sends_direct_command(fake_fw) -> None:
    app = PicoTuiApp(port=fake_fw.slave_name, mode="sensor", enable_file_log=False)
    async with app.run_test(size=(150, 48)) as pilot:
        for _ in range(10):
            await asyncio.sleep(0.1)
            await pilot.pause()
        await pilot.press("f6")
        await pilot.pause()
        await pilot.click("#tel-once")
        await asyncio.sleep(0.25)
        await pilot.pause()
        assert "TELEMETRY ONCE" in fake_fw.received_commands


@pytest.mark.asyncio
async def test_f7_opens_fft_request_window() -> None:
    app = PicoTuiApp(demo=True, enable_file_log=False)
    async with app.run_test(size=(150, 48)) as pilot:
        await asyncio.sleep(0.6)
        await pilot.pause()
        await pilot.press("f7")
        await pilot.pause()
        assert type(app.screen).__name__ == "FftRequestScreen"

@pytest.mark.asyncio
async def test_direct_fft_flow_opens_frequency_chart(fake_fw) -> None:
    app = PicoTuiApp(port=fake_fw.slave_name, mode="sensor", enable_file_log=False)
    async with app.run_test(size=(150, 50)) as pilot:
        for _ in range(10):
            await asyncio.sleep(0.1)
            await pilot.pause()
        await pilot.press("f7")
        await pilot.pause()
        await pilot.click("#fft-confirm")
        for _ in range(6):
            await asyncio.sleep(0.1)
            await pilot.pause()
        assert type(app.screen).__name__ == "FftViewScreen"
        chart_text = str(app.screen.query_one("#fft-ascii").render())
        assert "Magnitude" in chart_text
        assert "Hz" in chart_text

@pytest.mark.asyncio
async def test_f3_navigates_and_selects_another_sensor() -> None:
    app = PicoTuiApp(demo=True, enable_file_log=False)
    async with app.run_test(size=(150, 48)) as pilot:
        for _ in range(6):
            await asyncio.sleep(0.12)
            await pilot.pause()
        assert app.state_store.snapshot().selected_logical_id == "20.01"
        await pilot.press("f3")
        await pilot.pause()
        assert type(app.screen).__name__ == "NodeNavigatorScreen"
        await pilot.press("down")
        await pilot.press("enter")
        await pilot.pause()
        assert app.state_store.snapshot().selected_logical_id == "20.02"
        assert type(app.screen).__name__ != "NodeNavigatorScreen"


@pytest.mark.asyncio
async def test_quick_apply_button_sends_apply_and_waits_for_config_applied(fake_fw) -> None:
    app = PicoTuiApp(port=fake_fw.slave_name, mode="sensor", enable_file_log=False)
    async with app.run_test(size=(150, 48)) as pilot:
        for _ in range(10):
            await asyncio.sleep(0.1)
            await pilot.pause()
        await pilot.click("#quick-apply")
        await asyncio.sleep(0.35)
        await pilot.pause()
        assert "APPLY" in fake_fw.received_commands
        sensor = app.state_store.find_sensor("01.01")
        assert sensor is not None
        # O firmware falso responde CONFIG_APPLIED; o estado só então deixa SENT/QUEUED.
        assert sensor.configuration.transaction_state == "APPLIED"


def test_quick_config_has_only_apply_button() -> None:
    source = (Path(__file__).parents[1] / "pico_tui" / "widgets.py").read_text(encoding="utf-8")
    assert 'Button("Aplicar", id="quick-apply"' in source
    assert "quick-open-full" not in source
    assert "Abrir F4" not in source
