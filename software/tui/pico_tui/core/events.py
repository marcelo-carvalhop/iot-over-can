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
