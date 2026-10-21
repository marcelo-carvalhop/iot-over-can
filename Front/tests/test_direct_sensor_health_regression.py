"""Regressao: o host sintético nao pode produzir CRITICAL na TUI direta."""

import time

from pico_tui import presentation as pres
from pico_tui.core.models import (
    ConnectionMode,
    ConnectionState,
    DtcRecord,
    NodeStatus,
    Severity,
    TelemetrySample,
)
from pico_tui.core.state_store import StateStore


def _state(mode: ConnectionMode, node_type: str) -> StateStore:
    store = StateStore()
    store.set_connection(ConnectionState.READY, mode=mode)
    store.update_node(1, node_type=node_type, status=NodeStatus.ONLINE)
    store.update_telemetry(TelemetrySample(1, 1))
    return store


def test_usb_direct_healthy_sensor_is_not_critical_after_host_timeout():
    store = _state(ConnectionMode.SENSOR_DIRECT, 'DIRECT_SENSOR_HOST')
    host = store._state.nodes[1]
    assert host.node_type == 'DIRECT_SENSOR_HOST'  # TEL nao pode transformar host em CAN_NODE
    host.last_seen_monotonic = time.monotonic() - 45
    store.refresh_freshness()
    condition = pres.system_condition(store.snapshot())
    assert host.status == NodeStatus.ONLINE
    assert not host.incidents
    assert condition.level == pres.Level.NORMAL
    assert condition.modules_total == 0
    assert condition.sensors_total == 1
    assert not condition.attention


def test_usb_direct_still_detects_real_sensor_loss():
    store = _state(ConnectionMode.SENSOR_DIRECT, 'DIRECT_SENSOR_HOST')
    store._state.nodes[1].last_seen_monotonic = time.monotonic() - 45
    store._state.nodes[1].sensors[1].last_seen_monotonic = time.monotonic() - 45
    store.refresh_freshness()
    condition = pres.system_condition(store.snapshot())
    assert condition.level == pres.Level.CRITICAL
    assert [item.target for item in condition.attention] == ['sensor:01.01']
    assert 'Comunicação perdida' in condition.attention[0].reason


def test_usb_direct_still_detects_real_critical_dtc():
    store = _state(ConnectionMode.SENSOR_DIRECT, 'DIRECT_SENSOR_HOST')
    store.add_dtc(1, 1, DtcRecord(0x2002, Severity.CRITICAL))
    condition = pres.system_condition(store.snapshot())
    assert condition.level == pres.Level.CRITICAL
    assert any('0x2002' in item.reason for item in condition.attention)


def test_can_gateway_liveness_is_unchanged():
    store = _state(ConnectionMode.GATEWAY_CAN, 'CAN_NODE')
    node = store._state.nodes[1]
    node.last_seen_monotonic = time.monotonic() - 45
    store.refresh_freshness()
    condition = pres.system_condition(store.snapshot())
    assert node.status == NodeStatus.LOST
    assert condition.level == pres.Level.CRITICAL
    assert any(item.target == 'node:01' for item in condition.attention)
