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
    """Estados de aquisição aceitos pelo protocolo atual.

    POLLING é o modo oficial e saudável. DRDY permanece apenas para
    compatibilidade com logs antigos e experimentos futuros.
    """

    UNKNOWN = "UNKNOWN"
    POLLING = "POLLING"
    SIM = "SIM"
    IDLE = "IDLE"
    DRDY = "DRDY"
    # Sensor acústico: fluxo contínuo do INMP441 por I2S e DMA.
    I2S = "I2S"

    # Aliases de compatibilidade com versões anteriores da TUI.
    SIMULATED = "SIM"
    STOPPED = "IDLE"


class SensorMode(str, Enum):
    """Modos de operação dos perfis de sensor.

    O número do modo no firmware só tem significado junto com o perfil; aqui
    os nomes são únicos: ROTATING/STRUCTURAL/SEISMIC pertencem ao sensor de
    vibração e LEVEL/MACHINE/EVENTS ao sensor acústico (INMP441). IDLE vale
    para os dois.
    """

    IDLE = "IDLE"
    ROTATING = "ROTATING"
    STRUCTURAL = "STRUCTURAL"
    SEISMIC = "SEISMIC"
    LEVEL = "LEVEL"
    MACHINE = "MACHINE"
    EVENTS = "EVENTS"
    UNKNOWN = "UNKNOWN"


VIBRATION_MODES = frozenset({"ROTATING", "STRUCTURAL", "SEISMIC"})
ACOUSTIC_MODES = frozenset({"LEVEL", "MACHINE", "EVENTS"})


def profile_for_mode(mode: object) -> str:
    """Perfil a que um nome de modo pertence; "" para IDLE e desconhecidos."""

    name = str(getattr(mode, "value", mode) or "").upper()
    if name in VIBRATION_MODES:
        return "VIBRATION"
    if name in ACOUSTIC_MODES:
        return "ACOUSTIC"
    return ""


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
class AcousticMetrics:
    """Relatório de um segundo do sensor acústico (perfil ACOUSTIC).

    Níveis em dB re 20 µPa. ``None`` marca o que o modo não calcula ou o
    que o firmware não informou. As bandas de oitava cobrem 31,5 Hz a 16 kHz
    (ISO 266), sem ponderação.
    """

    interval_ms: int | None = None
    coverage_pct: int | None = None
    dsp_load_pct: int | None = None
    channel: str | None = None
    spectrum_valid: bool | None = None
    overload: bool | None = None
    mic_fault: bool | None = None
    laeq_db: float | None = None
    lceq_db: float | None = None
    lzeq_db: float | None = None
    lzpeak_db: float | None = None
    laf_db: float | None = None
    lafmax_db: float | None = None
    lafmin_db: float | None = None
    crest: float | None = None
    kurtosis: float | None = None
    peak_hz: float | None = None
    centroid_hz: float | None = None
    flatness: float | None = None
    octave_db: list[float | None] = field(default_factory=list)
    # LEVEL
    leq_t_db: float | None = None
    l10_db: float | None = None
    l50_db: float | None = None
    l90_db: float | None = None
    dose_nr15_pct: float | None = None
    dose_nho01_pct: float | None = None
    elapsed_s: int | None = None
    dose_limit: bool | None = None
    # MACHINE
    tonal_db: float | None = None
    f0_hz: float | None = None
    harmonics: int | None = None
    anomaly_z: float | None = None
    baseline: str | None = None          # NONE, LEARNING ou READY
    baseline_pct: int | None = None
    anomaly: bool | None = None
    # EVENTS
    events_total: int | None = None
    events_interval: int | None = None
    event_active: bool | None = None
    sta_lta_db: float | None = None
    background_db: float | None = None
    event_class: str | None = None       # IMPACT, CRACK, TONAL, TRANSIENT
    event_peak_db: float | None = None
    event_sel_db: float | None = None
    event_duration_ms: int | None = None
    event_centroid_hz: float | None = None
    event_age_s: int | None = None


@dataclass(slots=True)
class AcousticEvent:
    """Evento sonoro detectado no modo EVENTS e registrado pela TUI."""

    event_class: str
    peak_db: float | None = None
    sel_db: float | None = None
    duration_ms: int | None = None
    centroid_hz: float | None = None
    count: int = 1                        # eventos somados quando vários chegam no mesmo relatório
    received_wall_time: float = field(default_factory=time.time)


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
    # Presente apenas na telemetria do sensor acústico.
    acoustic: AcousticMetrics | None = None

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
class Incident:
    """Intercorrência registrada para um sensor ou módulo CAN.

    ``key`` agrupa repetições do mesmo fenômeno: ocorrências com a mesma chave
    dentro da janela de coalescência atualizam ``count`` e ``last_wall_time``
    em vez de criar novas linhas, evitando que uma falha persistente inunde a
    lista exibida ao operador.
    """

    severity: Severity
    kind: str
    message: str
    key: str = ""
    first_wall_time: float = field(default_factory=time.time)
    last_wall_time: float = field(default_factory=time.time)
    count: int = 1


INCIDENT_HISTORY = 60
ACOUSTIC_EVENT_HISTORY = 200


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
    # Sensor acústico: estado do microfone, canal I2S em uso e plataforma.
    microphone_state: str | None = None
    audio_channel: str | None = None
    chip: str | None = None
    board: str | None = None


@dataclass(slots=True)
class SensorNode:
    parent_node_id: int
    child_id: int
    wireless_uuid: str = ""
    profile_id: str = ""
    firmware_version: str = ""
    protocol_version: str = ""
    status: NodeStatus = NodeStatus.UNKNOWN
    association_state: str = "UNBOUND"
    association_rssi_dbm: int | None = None
    # Plano de dados via Node CAN: NONE (nunca informado), DOWN, OFFERING,
    # HANDSHAKE, SECURE ou NO_KEY. Só SECURE transporta telemetria e comandos.
    data_link_state: str = "NONE"
    data_link_rssi_dbm: int | None = None
    data_link_session_age_s: int = 0
    data_link_rx_datagrams: int = 0
    data_link_auth_failures: int = 0
    data_link_replay_drops: int = 0
    data_link_lost_datagrams: int = 0
    data_link_rtt_ms: float | None = None
    data_link_rtt_max_ms: float | None = None
    # Reassociação automática: AUTO (o líder pode mover o sensor para outro
    # módulo), MANUAL ou UNKNOWN (firmware que não informa a política).
    failover_policy: str = "UNKNOWN"
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
    incidents: deque[Incident] = field(default_factory=lambda: deque(maxlen=INCIDENT_HISTORY))
    # Eventos sonoros registrados nesta sessão (sensor acústico, modo EVENTS).
    acoustic_events: deque[AcousticEvent] = field(default_factory=lambda: deque(maxlen=ACOUSTIC_EVENT_HISTORY))

    @property
    def logical_id(self) -> str:
        return f"{self.parent_node_id:02d}.{self.child_id:02d}"

    @property
    def wireless_associated(self) -> bool:
        return self.association_state.upper() != "UNBOUND"

    @property
    def has_data_plane(self) -> bool:
        """Sessão autenticada ativa entre o sensor e o Node responsável."""

        return self.data_link_state.upper() == "SECURE"

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

    # Intercorrências do próprio módulo (comunicação, DTC de módulo, comandos).
    incidents: deque[Incident] = field(default_factory=lambda: deque(maxlen=INCIDENT_HISTORY))

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
