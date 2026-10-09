"""Acumula as métricas da rede a partir dos eventos de protocolo.

A Probe 00 mede o barramento (BUS_STATS, ID_STATS); os Nodes informam o
enlace com cada sensor (WIRELESS_LINK); a própria TUI observa a chegada da
telemetria, das confirmações de comando e os fatos da coordenação (sinal de
presença, eleições, reassociações). Este serviço reúne tudo em um retrato
que as telas e a exportação consomem.
"""
from __future__ import annotations

import time
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass, field

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    BusStatsReceived,
    CommandAck,
    ConnectionOpened,
    IdStatsReceived,
    LegacyHeartbeatReceived,
    NetworkEventReceived,
    PhysicalNodeReceived,
    TelemetryReceived,
    WirelessAssociationReceived,
    WirelessFailoverReceived,
    WirelessLinkReceived,
)
from pico_tui.metrics import rta
from pico_tui.metrics.catalog import TEST_LOAD_HIGH_ID, TEST_LOAD_LOW_ID, describe

BUS_HISTORY = 300          # janelas de 1 s
INTERVAL_HISTORY = 120     # intervalos de telemetria por sensor
COMMAND_HISTORY = 200
EVENT_HISTORY = 60
# Um identificador sem tráfego por este tempo deixa a análise.
ID_STALE_SECONDS = 20.0
# Teto de ocupação usado como referência de projeto para barramentos CAN com
# tráfego disparado por eventos.
GUIDELINE_UTILIZATION = 0.40

Clock = Callable[[], float]


@dataclass(slots=True)
class BusSample:
    t: float
    window_ms: int
    frames: int
    bits: int
    load_percent: float
    peak_percent: float
    rx_errors: int
    tx_errors: int
    error_flags: int
    rx_queue_peak: int
    ids: int
    transfers_ok: int
    transfers_err: int
    own_frames: int
    untracked: int


@dataclass(slots=True)
class IdSample:
    can_id: int
    name: str
    group: str
    t: float = 0.0
    window_ms: int = 0
    count: int = 0
    dlc: int = 0
    bits: int = 0
    dt_min_us: int = 0
    dt_avg_us: int = 0
    dt_max_us: int = 0
    # Acumulados na sessão.
    total_frames: int = 0
    session_dt_min_us: int = 0
    session_dt_max_us: int = 0
    windows: int = 0

    @property
    def jitter_us(self) -> int:
        return max(0, self.dt_max_us - self.dt_min_us)

    @property
    def rate_hz(self) -> float:
        return 1000.0 * self.count / self.window_ms if self.window_ms else 0.0


@dataclass(slots=True)
class SensorLinkMetrics:
    logical_id: str
    wireless_uuid: str = ""
    link_state: str = "NONE"
    rssi_dbm: int | None = None
    rtt_ms: float | None = None
    rtt_max_ms: float | None = None
    session_age_s: int = 0
    sessions: int = 0                  # vezes em que o enlace passou a SECURE
    last_secure_t: float | None = None
    last_down_t: float | None = None
    last_recovery_s: float | None = None   # última duração sem sessão
    auth_failures: int = 0
    replay_drops: int = 0
    node_lost_datagrams: int = 0
    telemetry_count: int = 0
    telemetry_lost: int = 0
    last_sequence: int | None = None
    last_telemetry_t: float | None = None
    intervals_s: deque[float] = field(default_factory=lambda: deque(maxlen=INTERVAL_HISTORY))
    max_gap_s: float = 0.0

    @property
    def interval_avg_s(self) -> float | None:
        return sum(self.intervals_s) / len(self.intervals_s) if self.intervals_s else None

    @property
    def interval_min_s(self) -> float | None:
        return min(self.intervals_s) if self.intervals_s else None

    @property
    def interval_max_s(self) -> float | None:
        return max(self.intervals_s) if self.intervals_s else None

    @property
    def loss_percent(self) -> float:
        total = self.telemetry_count + self.telemetry_lost
        return 100.0 * self.telemetry_lost / total if total else 0.0


@dataclass(slots=True)
class CoordinationMetrics:
    leader: int | None = None
    heartbeat_period_ms: int | None = None
    heartbeats: int = 0
    last_heartbeat_t: float | None = None
    max_heartbeat_gap_s: float = 0.0
    leader_changes: int = 0
    elections: int = 0
    leader_failures: int = 0
    joins: int = 0
    failovers: int = 0
    nodes_in_fault: set[int] = field(default_factory=set)


@dataclass(slots=True)
class TimelineEvent:
    t: float
    wall: float
    kind: str
    text: str


@dataclass(slots=True)
class CommandRtt:
    samples: deque[float] = field(default_factory=lambda: deque(maxlen=COMMAND_HISTORY))
    rejected: int = 0

    @property
    def count(self) -> int:
        return len(self.samples)

    @property
    def minimum(self) -> float | None:
        return min(self.samples) if self.samples else None

    @property
    def average(self) -> float | None:
        return sum(self.samples) / len(self.samples) if self.samples else None

    @property
    def maximum(self) -> float | None:
        return max(self.samples) if self.samples else None

    @property
    def p95(self) -> float | None:
        if not self.samples:
            return None
        ordered = sorted(self.samples)
        return ordered[min(len(ordered) - 1, int(0.95 * len(ordered)))]


@dataclass(slots=True)
class MetricsSnapshot:
    now: float
    started: float
    bus: BusSample | None
    bus_history: list[BusSample]
    max_load_percent: float
    max_peak_percent: float
    max_rx_errors: int
    max_tx_errors: int
    total_frames: int
    total_bits: int
    ids: list[IdSample]
    links: list[SensorLinkMetrics]
    coordination: CoordinationMetrics
    commands: CommandRtt
    timeline: list[TimelineEvent]
    analysis: rta.Analysis | None
    capacity: rta.Capacity | None            # limite teórico: todos os prazos cumpridos
    capacity_guideline: rta.Capacity | None  # com teto de ocupação (GUIDELINE_UTILIZATION)
    bitrate: int

    @property
    def has_bus_data(self) -> bool:
        return self.bus is not None

    @property
    def error_state(self) -> str:
        """Estado de confinamento de falhas do controlador da Probe (ISO 11898-1)."""

        if self.bus is None:
            return "UNKNOWN"
        if self.bus.error_flags & 0x20:      # TXBO
            return "BUS_OFF"
        if self.bus.rx_errors >= 128 or self.bus.tx_errors >= 128:
            return "ERROR_PASSIVE"
        if self.bus.rx_errors >= 96 or self.bus.tx_errors >= 96:
            return "ERROR_WARNING"
        return "ERROR_ACTIVE"


class MetricsService:
    def __init__(self, bus: EventBus, clock: Clock = time.monotonic, bitrate: int = rta.DEFAULT_BITRATE) -> None:
        self.bus = bus
        self.clock = clock
        self.bitrate = bitrate
        self._reset_state()
        for event_type, handler in (
            (BusStatsReceived, self._on_bus),
            (IdStatsReceived, self._on_id),
            (LegacyHeartbeatReceived, self._on_heartbeat),
            (NetworkEventReceived, self._on_network_event),
            (PhysicalNodeReceived, self._on_node),
            (TelemetryReceived, self._on_telemetry),
            (WirelessLinkReceived, self._on_link),
            (WirelessAssociationReceived, self._on_association),
            (WirelessFailoverReceived, self._on_failover),
            (CommandAck, self._on_ack),
            (ConnectionOpened, self._on_connection),
        ):
            bus.subscribe(event_type, handler)

    # ------------------------------------------------------------------

    def _reset_state(self) -> None:
        self.started = self.clock()
        self.bus_history: deque[BusSample] = deque(maxlen=BUS_HISTORY)
        self.max_load = 0.0
        self.max_peak = 0.0
        self.max_rx_errors = 0
        self.max_tx_errors = 0
        self.total_frames = 0
        self.total_bits = 0
        self.ids: dict[int, IdSample] = {}
        self.links: dict[str, SensorLinkMetrics] = {}
        self.coordination = CoordinationMetrics()
        self.commands = CommandRtt()
        self.timeline: deque[TimelineEvent] = deque(maxlen=EVENT_HISTORY)
        self._analysis_key: tuple | None = None
        self._analysis: rta.Analysis | None = None
        self._capacity: rta.Capacity | None = None
        self._capacity_guideline: rta.Capacity | None = None

    def reset(self) -> None:
        """Zera os acumulados da sessão (a tela oferece isso ao operador)."""

        self._reset_state()

    def set_bitrate(self, bitrate: int | None) -> None:
        if bitrate and bitrate > 0 and bitrate != self.bitrate:
            self.bitrate = bitrate
            self._analysis_key = None

    def _note(self, kind: str, text: str) -> None:
        self.timeline.append(TimelineEvent(self.clock(), time.time(), kind, text))

    def _link(self, parent: int, child: int) -> SensorLinkMetrics:
        logical_id = f"{parent:02d}.{child:02d}"
        entry = self.links.get(logical_id)
        if entry is None:
            entry = SensorLinkMetrics(logical_id)
            self.links[logical_id] = entry
        return entry

    # ------------------------------------------------------------ eventos

    def _on_connection(self, event: ConnectionOpened) -> None:
        self._reset_state()

    def _on_bus(self, event: BusStatsReceived) -> None:
        sample = BusSample(
            t=self.clock(),
            window_ms=event.window_ms,
            frames=event.frames,
            bits=event.bits,
            load_percent=event.load_percent,
            peak_percent=event.peak_percent,
            rx_errors=event.rx_errors,
            tx_errors=event.tx_errors,
            error_flags=event.error_flags,
            rx_queue_peak=event.rx_queue_peak,
            ids=event.ids,
            transfers_ok=event.transfers_ok,
            transfers_err=event.transfers_err,
            own_frames=event.own_frames,
            untracked=event.untracked,
        )
        self.bus_history.append(sample)
        self.max_load = max(self.max_load, sample.load_percent)
        self.max_peak = max(self.max_peak, sample.peak_percent)
        self.max_rx_errors = max(self.max_rx_errors, sample.rx_errors)
        self.max_tx_errors = max(self.max_tx_errors, sample.tx_errors)
        self.total_frames += sample.frames
        self.total_bits += sample.bits

    def _on_id(self, event: IdStatsReceived) -> None:
        entry = self.ids.get(event.can_id)
        if entry is None:
            info = describe(event.can_id)
            entry = IdSample(event.can_id, info.name, info.group)
            self.ids[event.can_id] = entry
        entry.t = self.clock()
        entry.window_ms = event.window_ms
        entry.count = event.count
        entry.dlc = event.dlc
        entry.bits = event.bits
        entry.dt_min_us = event.dt_min_us
        entry.dt_avg_us = event.dt_avg_us
        entry.dt_max_us = event.dt_max_us
        entry.total_frames += event.count
        entry.windows += 1
        if event.count >= 2:
            if entry.session_dt_min_us == 0 or event.dt_min_us < entry.session_dt_min_us:
                entry.session_dt_min_us = event.dt_min_us
            entry.session_dt_max_us = max(entry.session_dt_max_us, event.dt_max_us)

    def _on_heartbeat(self, event: LegacyHeartbeatReceived) -> None:
        coordination = self.coordination
        now = self.clock()
        if coordination.last_heartbeat_t is not None:
            coordination.max_heartbeat_gap_s = max(coordination.max_heartbeat_gap_s, now - coordination.last_heartbeat_t)
        coordination.last_heartbeat_t = now
        coordination.heartbeats += 1
        coordination.heartbeat_period_ms = event.period_ms
        if event.leader and event.leader != coordination.leader:
            if coordination.leader is not None:
                coordination.leader_changes += 1
                self._note("LEADER", f"Líder passou do Módulo {coordination.leader:02d} para o Módulo {event.leader:02d}")
            coordination.leader = event.leader

    def _on_network_event(self, event: NetworkEventReceived) -> None:
        coordination = self.coordination
        if event.kind == "LEADER_FAILURE_DETECTED":
            coordination.leader_failures += 1
            self._note("LEADER", f"Probe 00 detectou a falta do líder Módulo {event.node_id:02d}")
        elif event.kind == "ELECTION_REQUESTED":
            coordination.elections += 1
            self._note("ELECTION", "Eleição solicitada")
        elif event.kind == "JOIN_OBSERVED":
            coordination.joins += 1
            self._note("JOIN", f"Módulo {event.node_id:02d} pediu para entrar na rede")

    def _on_node(self, event: PhysicalNodeReceived) -> None:
        if event.parent_node_id == 0:
            return
        state = str(event.payload.get("STATE", "")).upper()
        faults = self.coordination.nodes_in_fault
        if state == "FALHA":
            if event.parent_node_id not in faults:
                faults.add(event.parent_node_id)
                self._note("FAULT", f"Módulo {event.parent_node_id:02d} marcado em falha")
        elif state in {"ONLINE", "ATIVO", "LEADER", "LIDER"}:
            faults.discard(event.parent_node_id)

    def _on_telemetry(self, event: TelemetryReceived) -> None:
        sample = event.sample
        entry = self._link(sample.parent_node_id, sample.child_id)
        now = self.clock()
        if entry.last_telemetry_t is not None:
            gap = now - entry.last_telemetry_t
            entry.intervals_s.append(gap)
            entry.max_gap_s = max(entry.max_gap_s, gap)
        entry.last_telemetry_t = now
        entry.telemetry_count += 1
        if sample.sequence is not None:
            if entry.last_sequence is not None:
                step = (sample.sequence - entry.last_sequence) & 0xFFFF
                if 1 < step < 0x8000:
                    entry.telemetry_lost += step - 1
            entry.last_sequence = sample.sequence

    def _on_link(self, event: WirelessLinkReceived) -> None:
        entry = self._link(event.parent_node_id, event.child_id)
        state = event.state.upper()
        now = self.clock()
        if state == "SECURE" and entry.link_state != "SECURE":
            entry.sessions += 1
            entry.last_secure_t = now
            entry.last_sequence = None   # o sensor pode ter reiniciado a contagem
            if entry.last_down_t is not None:
                entry.last_recovery_s = now - entry.last_down_t
                entry.last_down_t = None
        elif state != "SECURE" and entry.link_state == "SECURE":
            entry.last_down_t = now
        entry.link_state = state
        entry.rssi_dbm = event.rssi_dbm
        entry.rtt_ms = event.rtt_ms
        entry.rtt_max_ms = event.rtt_max_ms
        entry.session_age_s = event.session_age_s
        entry.auth_failures = event.auth_failures
        entry.replay_drops = event.replay_drops
        entry.node_lost_datagrams = event.lost_datagrams

    def _on_association(self, event: WirelessAssociationReceived) -> None:
        if event.child_id <= 0:
            return
        logical_id = f"{event.parent_node_id:02d}.{event.child_id:02d}"
        state = event.state.upper()
        if state == "UNBOUND":
            self.links.pop(logical_id, None)
            return
        if state == "REJECTED":
            return
        # O mesmo sensor sob outro módulo: o registro anterior deixa de valer.
        for other_id, other in list(self.links.items()):
            if other_id != logical_id and other.wireless_uuid and other.wireless_uuid == event.wireless_uuid:
                if state in {"ASSOCIATING", "BOUND", "ONLINE"}:
                    del self.links[other_id]
        self._link(event.parent_node_id, event.child_id).wireless_uuid = event.wireless_uuid

    def _on_failover(self, event: WirelessFailoverReceived) -> None:
        self.coordination.failovers += 1
        reason = {"OWNER_LOST": "módulo responsável fora do ar", "SENSOR_LOST": "sensor fora do alcance"}.get(
            event.reason, event.reason
        )
        self._note(
            "FAILOVER",
            f"{event.wireless_uuid}: Módulo {event.from_node_id:02d} → Módulo {event.to_node_id:02d} ({reason})",
        )

    def _on_ack(self, event: CommandAck) -> None:
        raw = event.payload.get("RTT_MS") if isinstance(event.payload, dict) else None
        state = event.state.upper()
        if state in {"REJECTED", "TIMEOUT"}:
            self.commands.rejected += 1
        if raw is None or state not in {"APPLIED", "ACK", "OK"}:
            return
        try:
            self.commands.samples.append(float(raw))
        except (TypeError, ValueError):
            pass

    # ------------------------------------------------------------ consulta

    def flows(self) -> list[rta.Flow]:
        now = self.clock()
        flows: list[rta.Flow] = []
        for entry in self.ids.values():
            if now - entry.t > ID_STALE_SECONDS:
                continue
            flow = rta.flow_from_observation(
                entry.can_id,
                entry.name,
                count=entry.count,
                window_ms=entry.window_ms,
                dlc=entry.dlc,
                dt_min_us=entry.dt_min_us,
                dt_avg_us=entry.dt_avg_us,
                dt_max_us=entry.dt_max_us,
                bitrate=self.bitrate,
                has_deadline=entry.can_id not in (TEST_LOAD_HIGH_ID, TEST_LOAD_LOW_ID),
            )
            if flow is not None:
                flows.append(flow)
        return flows

    def _analysis_cached(self) -> tuple[rta.Analysis | None, rta.Capacity | None, rta.Capacity | None]:
        flows = self.flows()
        key = (self.bitrate, tuple((f.can_id, f.dlc, round(f.period_us), f.burst, round(f.jitter_us)) for f in flows))
        if key != self._analysis_key:
            self._analysis_key = key
            if flows:
                self._analysis = rta.analyze(flows, self.bitrate)
                self._capacity = rta.capacity(flows, self.bitrate)
                self._capacity_guideline = rta.capacity(flows, self.bitrate, utilization_limit=GUIDELINE_UTILIZATION)
            else:
                self._analysis = None
                self._capacity = None
                self._capacity_guideline = None
        return self._analysis, self._capacity, self._capacity_guideline

    def snapshot(self) -> MetricsSnapshot:
        analysis, capacity, capacity_guideline = self._analysis_cached()
        history = list(self.bus_history)
        return MetricsSnapshot(
            now=self.clock(),
            started=self.started,
            bus=history[-1] if history else None,
            bus_history=history,
            max_load_percent=self.max_load,
            max_peak_percent=self.max_peak,
            max_rx_errors=self.max_rx_errors,
            max_tx_errors=self.max_tx_errors,
            total_frames=self.total_frames,
            total_bits=self.total_bits,
            ids=sorted(self.ids.values(), key=lambda item: item.can_id),
            links=sorted(self.links.values(), key=lambda item: item.logical_id),
            coordination=self.coordination,
            commands=self.commands,
            timeline=list(self.timeline),
            analysis=analysis,
            capacity=capacity,
            capacity_guideline=capacity_guideline,
            bitrate=self.bitrate,
        )
