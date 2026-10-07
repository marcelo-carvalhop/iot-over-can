from __future__ import annotations

import time

from pico_tui import presentation as pres
from pico_tui.core.models import (
    AppState,
    ConnectionMode,
    ConnectionState,
    DtcRecord,
    NodeStatus,
    PhysicalNode,
    SensorNode,
    Severity,
    TelemetrySample,
)


def _state_with(sensor: SensorNode) -> AppState:
    state = AppState(connection_state=ConnectionState.READY, connection_mode=ConnectionMode.GATEWAY_CAN)
    node = PhysicalNode(parent_node_id=sensor.parent_node_id, status=NodeStatus.ONLINE, last_seen_monotonic=time.monotonic())
    node.sensors[sensor.child_id] = sensor
    state.nodes[node.parent_node_id] = node
    return state


def test_pack_segments_never_splits_a_segment_and_respects_width():
    lengths = [12, 9, 14, 5, 30]
    for width in (10, 20, 31, 80):
        lines = pres.pack_segments(lengths, width, gap=3)
        assert sorted(index for line in lines for index in line) == list(range(len(lengths)))
        for line in lines:
            used = sum(lengths[index] for index in line) + 3 * (len(line) - 1)
            assert used <= width or len(line) == 1


def test_sensor_condition_levels():
    sensor = SensorNode(20, 1, status=NodeStatus.ONLINE, last_seen_monotonic=time.monotonic())
    sensor.latest_telemetry = TelemetrySample(20, 1, rms=0.1)
    assert pres.sensor_condition(sensor).level == pres.Level.NORMAL

    sensor.active_dtcs[0x2002] = DtcRecord(0x2002, Severity.WARNING)
    condition = pres.sensor_condition(sensor)
    assert condition.level == pres.Level.ATTENTION
    assert "0x2002" in condition.summary

    sensor.status = NodeStatus.LOST
    assert pres.sensor_condition(sensor).level == pres.Level.CRITICAL

    fresh = SensorNode(20, 2)
    assert pres.sensor_condition(fresh).level == pres.Level.NO_DATA


def test_system_condition_counts_and_attention_order():
    warning = SensorNode(20, 1, status=NodeStatus.STALE, last_seen_monotonic=1.0)
    state = _state_with(warning)
    critical = SensorNode(20, 2, status=NodeStatus.LOST, last_seen_monotonic=1.0)
    state.nodes[20].sensors[2] = critical
    condition = pres.system_condition(state)
    assert condition.level == pres.Level.CRITICAL
    assert condition.headline == "1 equipamento em estado crítico"
    assert [item.target for item in condition.attention] == ["sensor:20.02", "sensor:20.01"]

    state.network.bus_off = True
    assert pres.system_condition(state).headline == "Barramento CAN em bus-off"


def test_disconnected_is_not_an_alarm():
    condition = pres.system_condition(AppState())
    assert condition.level == pres.Level.NO_DATA
    assert condition.connected is False


def test_formatting_uses_brazilian_conventions():
    assert pres.fmt_number(0.12345, 3, "g") == "0,123 g"
    assert pres.fmt_number(None) == pres.MISSING
    assert pres.fmt_int(1234567) == "1.234.567"
    assert pres.fmt_age(0.2) == "agora"
    assert pres.fmt_age(12) == "há 12 s"
    assert pres.fmt_age(130) == "há 2 min"


def test_ascii_symbols_can_replace_unicode():
    try:
        pres.use_ascii_symbols(True)
        assert pres.symbol(pres.Level.CRITICAL) == "X"
        assert all(ord(char) < 128 for char in "".join(pres.symbol(level) for level in pres.Level))
    finally:
        pres.use_ascii_symbols(False)
    assert pres.symbol(pres.Level.CRITICAL) == "■"
