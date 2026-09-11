from __future__ import annotations

import time
from collections import deque
from dataclasses import dataclass, field
from enum import Enum
from typing import Any


class ConnectionState(str, Enum):
    DISCONNECTED = "DISCONNECTED"
    CONNECTING = "CONNECTING"
    CONNECTED = "CONNECTED"
    SYNCHRONIZING = "SYNCHRONIZING"
    READY = "READY"
    DEGRADED = "DEGRADED"
    RECONNECTING = "RECONNECTING"
    INCOMPATIBLE = "INCOMPATIBLE"


class ConnectionMode(str, Enum):
    AUTO = "AUTO"
    GATEWAY_CAN = "GATEWAY_CAN"
    SENSOR_DIRECT = "SENSOR_DIRECT"
    DEMO = "DEMO"
    UNKNOWN = "UNKNOWN"


class NodeStatus(str, Enum):
    ONLINE = "ONLINE"
    AGING = "AGING"
    STALE = "STALE"
    LOST = "LOST"
    OFFLINE = "OFFLINE"
    UNKNOWN = "UNKNOWN"


class AcquisitionMode(str, Enum):
    """Estados de aquisição aceitos pela baseline atual.

    POLLING é o modo oficial e saudável. DRDY permanece apenas para
    compatibilidade com logs antigos e experimentos futuros.
    """

    UNKNOWN = "UNKNOWN"
    POLLING = "POLLING"
    SIM = "SIM"
    IDLE = "IDLE"
    DRDY = "DRDY"

    # Aliases de compatibilidade com versões anteriores da TUI.
    SIMULATED = "SIM"
    STOPPED = "IDLE"


class SensorMode(str, Enum):
    IDLE = "IDLE"
    ROTATING = "ROTATING"
    STRUCTURAL = "STRUCTURAL"
    SEISMIC = "SEISMIC"
    UNKNOWN = "UNKNOWN"


class Severity(str, Enum):
    INFO = "INFO"
    WARNING = "WARNING"
    CRITICAL = "CRITICAL"


class DataQuality(str, Enum):
    REAL = "REAL"
    SIMULATED = "SIMULATED"
    PLACEHOLDER = "PLACEHOLDER"
    DEGRADED = "DEGRADED"
    INVALID = "INVALID"
    STALE = "STALE"
    LOST = "LOST"
    UNKNOWN = "UNKNOWN"


@dataclass(slots=True)
class BatteryInfo:
    present: bool = False
    percentage: float | None = None
    voltage_v: float | None = None
    current_ma: float | None = None
    source: str = "NOT_INSTRUMENTED"
    valid: bool = False
    charging: bool | None = None


@dataclass(slots=True)
class TelemetrySample:
    parent_node_id: int
    child_id: int
    received_monotonic: float = field(default_factory=time.monotonic)
    received_wall_time: float = field(default_factory=time.time)
    sensor_timestamp_ms: int | None = None
    gateway_timestamp_ms: int | None = None
    can_timestamp_ms: int | None = None
    sequence: int | None = None
    mode: SensorMode = SensorMode.UNKNOWN
    acquisition_mode: AcquisitionMode = AcquisitionMode.UNKNOWN
    sample_rate_requested_hz: float | None = None
    sample_rate_effective_hz: float | None = None
    window_type: str | None = None
    window_size: int | None = None
    rms: float | None = None
    rms_unit: str = "g"
    kurtosis: float | None = None
    crest_factor: float | None = None
    peak_frequency_hz: float | None = None
    peak_amplitude: float | None = None
    spectral_entropy: float | None = None
    fft_valid: bool | None = None
    axis: str | None = None
    ppv_mm_s: float | None = None
    stalta_triggered: bool | None = None
    clipping: bool | None = None
    dtc_code: int | None = None
    dtc_count: int | None = None
    quality: DataQuality = DataQuality.UNKNOWN
    battery: BatteryInfo = field(default_factory=BatteryInfo)
    raw: dict[str, Any] = field(default_factory=dict)

    @property
    def logical_id(self) -> str:
        return f"{self.parent_node_id:02d}.{self.child_id:02d}"


@dataclass(slots=True)
class SpectrumSample:
    parent_node_id: int
    child_id: int
    magnitudes: list[float]
    received_wall_time: float = field(default_factory=time.time)
    transfer_id: int | None = None
    sample_rate_hz: float | None = None
    fft_size: int | None = None
    window_type: str | None = None
    magnitude_unit: str = "raw"
    valid: bool = True

    @property
    def logical_id(self) -> str:
        return f"{self.parent_node_id:02d}.{self.child_id:02d}"


@dataclass(slots=True)
class DtcRecord:
    code: int
    severity: Severity = Severity.INFO
    symptom: int | None = None
    timestamp_ms: int | None = None
    received_wall_time: float = field(default_factory=time.time)
    active: bool = True
    acknowledged: bool = False
    occurrence_count: int = 1
    raw: dict[str, Any] = field(default_factory=dict)


@dataclass(slots=True)
class SensorConfiguration:
    mode: SensorMode = SensorMode.UNKNOWN
    sample_rate_requested_hz: float | None = None
    sample_rate_effective_hz: float | None = None
    window_type: str | None = None
    window_size: int | None = None
    stalta_threshold: float | None = None
    calibration_gain: float | None = None
    telemetry_rate_hz: float | None = None
    simulation_enabled: bool = False
    transaction_state: str = "IDLE"


@dataclass(slots=True)
class SensorHealth:
    mpu_present: bool | None = None
    drdy_irq_count: int | None = None
    drdy_missed_count: int | None = None
    network_state: str = "UNKNOWN"
    telemetry_enabled: bool = False
    telemetry_period_ms: int | None = None
    drdy_enabled: bool | None = None


@dataclass(slots=True)
class SensorNode:
    parent_node_id: int
    child_id: int
    wireless_uuid: str = ""
    profile_id: str = ""
    firmware_version: str = ""
    protocol_version: str = ""
    status: NodeStatus = NodeStatus.UNKNOWN
    acquisition_mode: AcquisitionMode = AcquisitionMode.UNKNOWN
    sensor_mode: SensorMode = SensorMode.UNKNOWN
    quality: DataQuality = DataQuality.UNKNOWN
    last_seen_monotonic: float = 0.0
    rx_count: int = 0
    lost_count: int = 0
    duplicate_count: int = 0
    out_of_order_count: int = 0
    last_sequence: int | None = None
    reported_dtc_count: int | None = None
    active_dtcs: dict[int, DtcRecord] = field(default_factory=dict)
    configuration: SensorConfiguration = field(default_factory=SensorConfiguration)
    health: SensorHealth = field(default_factory=SensorHealth)
    latest_telemetry: TelemetrySample | None = None
    latest_fft: SpectrumSample | None = None
    telemetry_history: deque[TelemetrySample] = field(
        default_factory=lambda: deque(maxlen=2000),
    )

    @property
    def logical_id(self) -> str:
        return f"{self.parent_node_id:02d}.{self.child_id:02d}"

    @property
    def loss_percent(self) -> float:
        total = self.rx_count + self.lost_count
        return (100.0 * self.lost_count / total) if total else 0.0

    @property
    def highest_severity(self) -> Severity | None:
        active = [record.severity for record in self.active_dtcs.values() if record.active]
        if Severity.CRITICAL in active:
            return Severity.CRITICAL
        if Severity.WARNING in active:
            return Severity.WARNING
        if Severity.INFO in active:
            return Severity.INFO
        return None

    @property
    def active_dtc_count(self) -> int:
        return sum(1 for record in self.active_dtcs.values() if record.active)



@dataclass(slots=True)
class WirelessCandidate:
    wireless_uuid: str
    reporter_node_id: int
    profile_id: str = "UNKNOWN"
    rssi_dbm: int = -127
    protocol_version: str = ""
    last_seen_monotonic: float = field(default_factory=time.monotonic)


@dataclass(slots=True)
class PhysicalNode:
    parent_node_id: int
    node_type: str = "CAN_NODE"
    role: str = "UNKNOWN"
    firmware_version: str = ""
    protocol_version: str = ""
    uptime_ms: int | None = None
    can_state: str = "UNKNOWN"
    wifi_state: str = "UNKNOWN"
    status: NodeStatus = NodeStatus.UNKNOWN
    last_seen_monotonic: float = 0.0
    rx_count: int = 0
    tx_count: int = 0
    error_count: int = 0
    active_dtcs: dict[int, DtcRecord] = field(default_factory=dict)

    # Capacidades são propriedades do módulo; role (LEADER/FOLLOWER) é dinâmica
    # e não altera o conjunto de capacidades de hardware/firmware.
    capabilities: set[str] = field(default_factory=lambda: {"CAN"})
    local_sensor_profile: str = "NONE"
    local_sensor_value: int | None = None
    local_sensor_enabled: bool | None = None
    local_sensor_last_round: int | None = None
    local_sensor_last_seen_monotonic: float = 0.0

    wireless_discovery_state: str = "NOT_IMPLEMENTED"
    wireless_ap_state: str = "NOT_IMPLEMENTED"
    wireless_candidate_count: int = 0
    wireless_candidates: dict[str, WirelessCandidate] = field(default_factory=dict)

    # Sensores wireless associados ao nó.
    sensors: dict[int, SensorNode] = field(default_factory=dict)

    # Campos legados mantidos para compatibilidade de logs antigos.
    legacy_last_value: str = "-"
    legacy_last_round: int | None = None

    @property
    def active_dtc_count(self) -> int:
        return sum(1 for record in self.active_dtcs.values() if record.active)


@dataclass(slots=True)
class GatewayInfo:
    node_id: int = 0
    firmware_version: str = ""
    protocol_version: str = ""
    serial_port: str = ""
    uptime_ms: int | None = None
    can_state: str = "UNKNOWN"
    wifi_state: str = "UNKNOWN"
    arbitration_bitrate: int | None = None
    data_bitrate: int | None = None
    last_seen_monotonic: float = 0.0


@dataclass(slots=True)
class CanFrameRecord:
    can_id: int
    data: bytes
    timestamp_ms: int | None = None
    direction: str = "RX"
    fd: bool = True
    brs: bool = True
    esi: bool = False
    received_wall_time: float = field(default_factory=time.time)


@dataclass(slots=True)
class NetworkStats:
    frames_rx: int = 0
    frames_tx: int = 0
    bytes_rx: int = 0
    bytes_tx: int = 0
    crc_errors: int = 0
    parse_errors: int = 0
    serial_frame_errors: int = 0
    bus_off: bool = False
    error_passive: bool = False
    error_warning: bool = False
    utilization_percent: float | None = None
    active_transfers: int = 0
    recent_frames: deque[CanFrameRecord] = field(default_factory=lambda: deque(maxlen=250))


@dataclass(slots=True)
class AppState:
    connection_state: ConnectionState = ConnectionState.DISCONNECTED
    connection_mode: ConnectionMode = ConnectionMode.UNKNOWN
    port: str = ""
    session_id: str = ""
    gateway: GatewayInfo = field(default_factory=GatewayInfo)
    nodes: dict[int, PhysicalNode] = field(default_factory=dict)
    network: NetworkStats = field(default_factory=NetworkStats)
    selected_logical_id: str | None = None
    selected_node_id: int | None = None
    last_action: str = ""
    refresh_paused: bool = False
