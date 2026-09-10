from pathlib import Path


ROOT = Path(__file__).resolve().parents[1] / "pico_tui"


def test_vibration_panels_start_hidden():
    source = (ROOT / "app.py").read_text()
    assert "vibration_panel.display = False" in source
    assert "vibration_config.display = False" in source


def test_vibration_panels_require_vibration_profile():
    source = (ROOT / "app.py").read_text()
    assert 'selected.profile_id or "").upper() == "VIBRATION"' in source


def test_physical_node_has_own_detail_screen():
    source = (ROOT / "screens.py").read_text()
    assert "class CanNodeDetailScreen" in source
    assert "SENSOR LOCAL" in source
    assert "WIRELESS" in source
    assert "SENSORES WIRELESS ASSOCIADOS" in source


def test_vibration_entries_are_contextual_in_main_menu():
    source = (ROOT / "screens.py").read_text()
    assert 'if self.selected_profile == "VIBRATION":' in source
    assert '("Sensor de vibração / Telemetria", "telemetry")' in source
    assert '("Sensor de vibração / FFT", "fft")' in source
