from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from pico_tui.core.models import ConnectionMode, DtcRecord, SpectrumSample, TelemetrySample


@dataclass(slots=True)
class Event:
    """Base dos eventos de domínio."""


@dataclass(slots=True)
class LogEvent(Event):
    level: str
    message: str
    source: str = "TUI"
    data: dict[str, Any] = field(default_factory=dict)


@dataclass(slots=True)
class RawLineReceived(Event):
    line: str


@dataclass(slots=True)
class ConnectionOpened(Event):
    port: str


@dataclass(slots=True)
class ConnectionClosed(Event):
    port: str
    reason: str = ""


@dataclass(slots=True)
class ConnectionModeDetected(Event):
    mode: ConnectionMode


@dataclass(slots=True)
class GatewayDetected(Event):
    firmware_version: str = ""
    protocol_version: str = ""


@dataclass(slots=True)
class GatewayStatusReceived(Event):
    payload: dict[str, Any]


@dataclass(slots=True)
class PhysicalNodeReceived(Event):
    parent_node_id: int
    payload: dict[str, Any]


@dataclass(slots=True)
class LocalNodeTelemetryReceived(Event):
    parent_node_id: int
    value: int
    round_number: int | None = None
    enabled: bool | None = None
    profile_id: str = "DEMO_BYTE"



@dataclass(slots=True)
class WirelessCandidateReceived(Event):
    reporter_node_id: int
    wireless_uuid: str
    profile_id: str
    rssi_dbm: int
    protocol_version: str = ""


@dataclass(slots=True)
class WirelessAssociationReceived(Event):
    parent_node_id: int
    child_id: int
    wireless_uuid: str
    profile_id: str
    state: str
    rssi_dbm: int
    protocol_version: str = ""
    # Política de reassociação e situação da sessão de dados (None quando o
    # firmware do Node é anterior e não as informa).
    failover_auto: bool | None = None
    session_secure: bool | None = None


@dataclass(slots=True)
class WirelessFailoverReceived(Event):
    """O líder reassociou um sensor a outro Node CAN."""

    wireless_uuid: str
    from_node_id: int
    to_node_id: int
    reason: str
    rssi_dbm: int | None = None


@dataclass(slots=True)
class WirelessLinkReceived(Event):
    """Estado do plano de dados Pico W <-> Node CAN publicado pelo Node responsável."""

    parent_node_id: int
    child_id: int
    state: str
    rssi_dbm: int | None = None
    session_age_s: int = 0
    rx_datagrams: int = 0
    auth_failures: int = 0
    replay_drops: int = 0
    lost_datagrams: int = 0
    stream_enabled: bool = False
    stream_period_ms: int | None = None
    # Tempo de ida e volta Node <-> sensor (PING/PONG); None = sem medida.
    rtt_ms: float | None = None
    rtt_max_ms: float | None = None


@dataclass(slots=True)
class WirelessConfigReceived(Event):
    """Configuração do sensor informada pelo Node (CURRENT, APPLIED ou REJECTED)."""

    parent_node_id: int
    child_id: int
    status: str
    payload: dict[str, Any]


@dataclass(slots=True)
class SensorStatusReceived(Event):
    parent_node_id: int
    child_id: int
    payload: dict[str, Any]


@dataclass(slots=True)
class DirectSensorVersionReceived(Event):
    protocol_version: str
    wireless_uuid: str


@dataclass(slots=True)
class DirectSensorStatusReceived(Event):
    payload: dict[str, Any]


@dataclass(slots=True)
class ConfigurationApplied(Event):
    parent_node_id: int
    child_id: int
    payload: dict[str, Any]


@dataclass(slots=True)
class TelemetryReceived(Event):
    sample: TelemetrySample


@dataclass(slots=True)
class SpectrumReceived(Event):
    sample: SpectrumSample


@dataclass(slots=True)
class DtcReceived(Event):
    parent_node_id: int
    child_id: int
    record: DtcRecord


@dataclass(slots=True)
class DtcCleared(Event):
    parent_node_id: int
    child_id: int
    code: int | None = None


@dataclass(slots=True)
class CommandAck(Event):
    command: str
    state: str = "ACK"
    transaction_id: str = ""
    payload: dict[str, Any] = field(default_factory=dict)


@dataclass(slots=True)
class CanFrameReceived(Event):
    can_id: int
    data: bytes
    timestamp_ms: int | None = None
    direction: str = "RX"
    fd: bool = True
    brs: bool = True
    esi: bool = False


@dataclass(slots=True)
class CrcErrorReceived(Event):
    crc_type: str
    parent_node_id: int | None = None
    child_id: int | None = None
    message_type: int | None = None
    detail: str = ""


@dataclass(slots=True)
class LegacyHeartbeatReceived(Event):
    leader: int
    round_number: int
    active_sensors: int
    mode: int
    period_ms: int


@dataclass(slots=True)
class FragmentReceived(Event):
    source: tuple[int, int]
    transfer_type: str
    transfer_id: int
    fragment_index: int
    fragment_count: int
    data: bytes
    expected_crc32: int | None = None
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass(slots=True)
class TransferCompleted(Event):
    source: tuple[int, int]
    transfer_type: str
    transfer_id: int
    payload: bytes
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass(slots=True)
class TransferFailed(Event):
    source: tuple[int, int]
    transfer_type: str
    transfer_id: int
    reason: str


# ---------------------------------------------------------------------------
# Métricas do barramento e ensaios
# ---------------------------------------------------------------------------


@dataclass(slots=True)
class BusStatsReceived(Event):
    """Janela de 1 s medida pela Probe 00."""

    probe_ms: int
    window_ms: int
    frames: int
    bits: int
    load_percent: float
    peak_percent: float
    rx_errors: int = 0
    tx_errors: int = 0
    error_flags: int = 0
    rx_queue_peak: int = 0
    ids: int = 0
    transfers_ok: int = 0
    transfers_err: int = 0
    own_frames: int = 0
    untracked: int = 0


@dataclass(slots=True)
class IdStatsReceived(Event):
    """Período de um identificador CAN em uma janela de 5 s."""

    can_id: int
    window_ms: int
    count: int
    dlc: int
    bits: int
    dt_min_us: int
    dt_avg_us: int
    dt_max_us: int


@dataclass(slots=True)
class FaultCommandSent(Event):
    """A Probe 00 transmitiu um comando de ensaio."""

    kind: str
    node_id: int
    duration_ms: int
    seq: int


@dataclass(slots=True)
class FaultEventReceived(Event):
    """Início, fim ou recusa de um ensaio de injeção de falha."""

    node_id: int
    kind: str
    state: str
    duration_ms: int = 0
    seq: int = 0
    payload: dict[str, Any] = field(default_factory=dict)


@dataclass(slots=True)
class NetworkEventReceived(Event):
    """Fato da coordenação da rede observado pela Probe 00.

    kinds: LEADER_FAILURE_DETECTED, ELECTION_REQUESTED, ELECTION_ANNOUNCE,
    LEADER_ANNOUNCED, LEADER_OBSERVED, JOIN_OBSERVED.
    """

    kind: str
    node_id: int = 0
    detail: str = ""
