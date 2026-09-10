from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "pico_tui"


def test_can_node_screen_is_live_and_actionable():
    source = (ROOT / "screens.py").read_text()
    assert "class CanNodeDetailScreen" in source
    assert "self.set_interval(0.25, self._refresh_live)" in source
    assert 'Button("DTC", id="can-node-dtc")' in source
    assert 'Button("Configurar", id="can-node-config")' in source
    assert "local_sensor_value" in source


def test_node_dtc_screen_exists():
    source = (ROOT / "screens.py").read_text()
    assert "class CanNodeDtcScreen" in source
    assert "Nenhum DTC de módulo reportado." in source
