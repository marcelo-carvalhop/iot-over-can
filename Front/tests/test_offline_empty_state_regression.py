from pathlib import Path


def test_quick_status_defines_sensor_count_before_empty_sensor_branch():
    source = (Path(__file__).resolve().parents[1] / "pico_tui" / "widgets.py").read_text()
    marker = "def refresh_state(self, state: AppState, sensor: SensorNode | None) -> None:"
    start = source.index(marker)
    block = source[start:start + 1800]
    assert "sensor_count = sum(len(node.sensors) for node in state.nodes.values())" in block
    assert block.index("sensor_count = sum(") < block.index("if sensor is None:")
