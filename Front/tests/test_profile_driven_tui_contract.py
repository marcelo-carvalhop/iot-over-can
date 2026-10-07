"""A interface especializada depende do perfil do sensor."""
from __future__ import annotations

from pico_tui.app import PicoTuiApp
from pico_tui.command_catalog import actions_for
from pico_tui.core.events import SensorStatusReceived
from pico_tui.screens import SensorScreen
from tests.conftest import settle


def test_vibration_only_actions_require_vibration_profile():
    generic = [action.key for action in actions_for("sensor", profile="TEMPERATURE")]
    vibration = [action.key for action in actions_for("sensor", profile="VIBRATION")]
    assert "sensor.fft" not in generic and "sensor.configure" not in generic
    assert "sensor.fft" in vibration and "sensor.configure" in vibration


async def test_unknown_profile_uses_generic_sensor_view(prefs_file) -> None:
    app = PicoTuiApp(enable_file_log=False, security_mode="off", preferences_path=prefs_file)
    async with app.run_test(size=(120, 40)) as pilot:
        await app.bus.publish(SensorStatusReceived(5, 1, {"STATE": "ONLINE", "PROFILE": "TEMPERATURE", "UUID": "0xBEEF"}))
        app.open_target("sensor:05.01")
        await settle(pilot, 3)
        assert isinstance(app.screen, SensorScreen)
        assert app.screen.query_one("#sec-metrics").display is False
        assert app.screen.query_one("#sec-raw").display is True
        hints = app.screen.query_one("#key-bar").source_text()
        assert "Espectro" not in hints


async def test_vibration_profile_shows_metrics_and_spectrum_key(prefs_file) -> None:
    app = PicoTuiApp(demo=True, enable_file_log=False, security_mode="off", preferences_path=prefs_file)
    async with app.run_test(size=(120, 40)) as pilot:
        await settle(pilot, 5)
        app.open_target("sensor:21.01")
        await settle(pilot, 3)
        assert app.screen.query_one("#sec-metrics").display is True
        assert "Espectro" in app.screen.query_one("#key-bar").source_text()
