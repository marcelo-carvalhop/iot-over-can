from __future__ import annotations

import threading
import time
from copy import deepcopy

from pico_tui.core.models import (
    AppState,
    CanFrameRecord,
    ConnectionMode,
    ConnectionState,
    DtcRecord,
    NodeStatus,
    PhysicalNode,
    SensorNode,
    SpectrumSample,
    TelemetrySample,
)


class StateStore:
    """Repositório central de estado; a UI consome apenas snapshots."""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._state = AppState()

    def snapshot(self) -> AppState:
        with self._lock:
            return deepcopy(self._state)

    def set_connection(
        self,
        state: ConnectionState,
        *,
        port: str | None = None,
        mode: ConnectionMode | None = None,
    ) -> None:
        with self._lock:
            self._state.connection_state = state
            if port is not None:
                self._state.port = port
                self._state.gateway.serial_port = port
            if mode is not None:
                self._state.connection_mode = mode

    def set_session_id(self, session_id: str) -> None:
        with self._lock:
            self._state.session_id = session_id

    def set_last_action(self, action: str) -> None:
        with self._lock:
            self._state.last_action = action

    def set_selected(self, logical_id: str | None) -> None:
        with self._lock:
            self._state.selected_logical_id = logical_id
            if logical_id and "." in logical_id:
                try:
                    self._state.selected_node_id = int(logical_id.split(".", 1)[0])
                except ValueError:
                    pass

    def set_selected_node(self, parent_node_id: int | None) -> None:
        with self._lock:
            self._state.selected_node_id = parent_node_id
            self._state.selected_logical_id = None

    def set_refresh_paused(self, paused: bool) -> None:
        with self._lock:
            self._state.refresh_paused = paused

    def update_gateway(self, **values: object) -> None:
        with self._lock:
            gateway = self._state.gateway
            for key, value in values.items():
                if hasattr(gateway, key):
                    setattr(gateway, key, value)
            gateway.last_seen_monotonic = time.monotonic()

    def ensure_node(self, parent_node_id: int, node_type: str = "UNKNOWN") -> PhysicalNode:
        with self._lock:
            node = self._state.nodes.get(parent_node_id)
            if node is None:
                node = PhysicalNode(parent_node_id=parent_node_id, node_type=node_type)
                self._state.nodes[parent_node_id] = node
            elif node_type != "UNKNOWN":
                node.node_type = node_type
            return node

    def add_node_capabilities(self, parent_node_id: int, *capabilities: str) -> None:
        with self._lock:
            node = self.ensure_node(parent_node_id)
            node.capabilities.update(cap for cap in capabilities if cap)

    def update_node(self, parent_node_id: int, **values: object) -> None:
        with self._lock:
            node = self.ensure_node(parent_node_id)
            for key, value in values.items():
                if hasattr(node, key):
                    setattr(node, key, value)
            node.last_seen_monotonic = time.monotonic()

    def ensure_sensor(
        self,
        parent_node_id: int,
        child_id: int,
        *,
        wireless_uuid: str = "",
    ) -> SensorNode:
        with self._lock:
            node = self.ensure_node(parent_node_id, "CAN_NODE")
            sensor = node.sensors.get(child_id)
            if sensor is None:
                sensor = SensorNode(
                    parent_node_id=parent_node_id,
                    child_id=child_id,
                    wireless_uuid=wireless_uuid,
                )
                node.sensors[child_id] = sensor
            elif wireless_uuid:
                sensor.wireless_uuid = wireless_uuid
            return sensor

    def update_sensor(self, parent_node_id: int, child_id: int, **values: object) -> None:
        with self._lock:
            sensor = self.ensure_sensor(parent_node_id, child_id)
            for key, value in values.items():
                if hasattr(sensor, key):
                    setattr(sensor, key, value)
            sensor.last_seen_monotonic = time.monotonic()

    def update_sensor_configuration(
        self,
        parent_node_id: int,
        child_id: int,
        **values: object,
    ) -> None:
        with self._lock:
            sensor = self.ensure_sensor(parent_node_id, child_id)
            for key, value in values.items():
                if hasattr(sensor.configuration, key):
                    setattr(sensor.configuration, key, value)

    def update_sensor_health(
        self,
        parent_node_id: int,
        child_id: int,
        **values: object,
    ) -> None:
        with self._lock:
            sensor = self.ensure_sensor(parent_node_id, child_id)
            for key, value in values.items():
                if hasattr(sensor.health, key):
                    setattr(sensor.health, key, value)

    def add_node_dtc(self, parent_node_id: int, record: DtcRecord) -> None:
        with self._lock:
            node = self.ensure_node(parent_node_id)
            existing = node.active_dtcs.get(record.code)
            if existing and existing.active:
                record.occurrence_count = existing.occurrence_count + 1
            node.active_dtcs[record.code] = record

    def clear_node_dtc(self, parent_node_id: int, code: int | None = None) -> None:
        with self._lock:
            node = self.ensure_node(parent_node_id)
            if code is None:
                for record in node.active_dtcs.values():
                    record.active = False
            elif code in node.active_dtcs:
                node.active_dtcs[code].active = False

    def update_network(self, **values: object) -> None:
        with self._lock:
            network = self._state.network
            for key, value in values.items():
                if hasattr(network, key):
                    setattr(network, key, value)

    def increment_network(
        self,
        *,
        frames_rx: int = 0,
        frames_tx: int = 0,
        bytes_rx: int = 0,
        bytes_tx: int = 0,
        crc_errors: int = 0,
        parse_errors: int = 0,
        serial_frame_errors: int = 0,
    ) -> None:
        with self._lock:
            network = self._state.network
            network.frames_rx += frames_rx
            network.frames_tx += frames_tx
            network.bytes_rx += bytes_rx
            network.bytes_tx += bytes_tx
            network.crc_errors += crc_errors
            network.parse_errors += parse_errors
            network.serial_frame_errors += serial_frame_errors

    def add_can_frame(self, frame: CanFrameRecord) -> None:
        with self._lock:
            self._state.network.recent_frames.append(frame)

    def update_telemetry(self, sample: TelemetrySample) -> SensorNode:
        with self._lock:
            sensor = self.ensure_sensor(sample.parent_node_id, sample.child_id)
            sensor.latest_telemetry = sample
            if sample.fft_valid is False:
                # Um espectro anterior não representa o buffer atual quando FFT_VALID=NO.
                sensor.latest_fft = None
            sensor.telemetry_history.append(sample)
            sensor.rx_count += 1
            sensor.last_seen_monotonic = sample.received_monotonic
            sensor.status = NodeStatus.ONLINE
            sensor.sensor_mode = sample.mode
            sensor.acquisition_mode = sample.acquisition_mode
            sensor.quality = sample.quality
            sensor.configuration.mode = sample.mode
            sensor.configuration.sample_rate_requested_hz = sample.sample_rate_requested_hz
            sensor.configuration.sample_rate_effective_hz = sample.sample_rate_effective_hz
            sensor.configuration.window_type = sample.window_type
            sensor.configuration.window_size = sample.window_size
            sensor.configuration.simulation_enabled = sample.acquisition_mode.value == "SIM" or sample.quality.value == "SIMULATED"
            if sample.dtc_count is not None:
                sensor.reported_dtc_count = sample.dtc_count
            return sensor

    def update_spectrum(self, sample: SpectrumSample) -> None:
        with self._lock:
            sensor = self.ensure_sensor(sample.parent_node_id, sample.child_id)
            sensor.latest_fft = sample
            sensor.last_seen_monotonic = time.monotonic()

    def add_dtc(self, parent_node_id: int, child_id: int, record: DtcRecord) -> None:
        with self._lock:
            sensor = self.ensure_sensor(parent_node_id, child_id)
            existing = sensor.active_dtcs.get(record.code)
            if existing and existing.active:
                record.occurrence_count = existing.occurrence_count + 1
            sensor.active_dtcs[record.code] = record

    def clear_dtc(self, parent_node_id: int, child_id: int, code: int | None = None) -> None:
        with self._lock:
            sensor = self.ensure_sensor(parent_node_id, child_id)
            if code is None:
                for record in sensor.active_dtcs.values():
                    record.active = False
            elif code in sensor.active_dtcs:
                sensor.active_dtcs[code].active = False

    def record_sequence(
        self,
        parent_node_id: int,
        child_id: int,
        *,
        sequence: int,
        lost: int = 0,
        duplicate: bool = False,
        out_of_order: bool = False,
    ) -> None:
        with self._lock:
            sensor = self.ensure_sensor(parent_node_id, child_id)
            if lost:
                sensor.lost_count += lost
            if duplicate:
                sensor.duplicate_count += 1
            if out_of_order:
                sensor.out_of_order_count += 1
            sensor.last_sequence = sequence

    def refresh_freshness(
        self,
        *,
        aging_after: float = 3.0,
        stale_after: float = 10.0,
        lost_after: float = 30.0,
    ) -> None:
        now = time.monotonic()
        with self._lock:
            for node in self._state.nodes.values():
                if node.last_seen_monotonic:
                    node_age = now - node.last_seen_monotonic
                    if node_age >= lost_after:
                        node.status = NodeStatus.LOST
                    elif node_age >= stale_after:
                        node.status = NodeStatus.STALE
                    elif node_age >= aging_after:
                        node.status = NodeStatus.AGING
                    else:
                        node.status = NodeStatus.ONLINE
                for sensor in node.sensors.values():
                    if not sensor.last_seen_monotonic:
                        sensor.status = NodeStatus.UNKNOWN
                        continue
                    age = now - sensor.last_seen_monotonic
                    if age >= lost_after:
                        sensor.status = NodeStatus.LOST
                    elif age >= stale_after:
                        sensor.status = NodeStatus.STALE
                    elif age >= aging_after:
                        sensor.status = NodeStatus.AGING
                    else:
                        sensor.status = NodeStatus.ONLINE

    def find_node(self, parent_node_id: int | None) -> PhysicalNode | None:
        if parent_node_id is None:
            return None
        with self._lock:
            node = self._state.nodes.get(parent_node_id)
            return deepcopy(node) if node else None

    def find_sensor(self, logical_id: str) -> SensorNode | None:
        with self._lock:
            for node in self._state.nodes.values():
                for sensor in node.sensors.values():
                    if sensor.logical_id == logical_id:
                        return deepcopy(sensor)
        return None

    def first_sensor_id(self) -> str | None:
        with self._lock:
            for parent_id in sorted(self._state.nodes):
                node = self._state.nodes[parent_id]
                for child_id in sorted(node.sensors):
                    return node.sensors[child_id].logical_id
        return None

    def summary(self) -> dict[str, object]:
        with self._lock:
            sensors = sum(len(node.sensors) for node in self._state.nodes.values())
            return {
                "nodes": len(self._state.nodes),
                "sensors": sensors,
                "connection_state": self._state.connection_state.value,
                "connection_mode": self._state.connection_mode.value,
            }
