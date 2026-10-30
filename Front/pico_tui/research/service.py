"""Acumula, ao vivo, as medições da instrumentação de pesquisa.

A Probe 00 publica os parâmetros da compilação (``[GW] CONFIG``), os trens de
eco (``ECHO_START``, ``ECHO``, ``ECHO_DONE``), o custo da criptografia e da
ponte Wi-Fi → CAN medido em cada Node (``WIRELESS_STATS``), o custo da
criptografia na própria Probe (``BENCH``) e a carga de fundo com semente
(``FAULT_EVENT kind=BUS_LOAD``). O sinal de presença do líder e a ocupação do
barramento vêm das linhas de sempre (``HEARTBEAT`` com ``t_us`` e
``BUS_STATS``).

Os números servem para acompanhar um experimento na bancada. A análise de uma
campanha (repetições, comparações, proveniência) continua com
``Pesquisa/iocpesquisa``; as definições das métricas são as mesmas.
"""
from __future__ import annotations

import time
from collections import deque
from dataclasses import dataclass, field

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    BusStatsReceived,
    ConnectionOpened,
    FaultEventReceived,
    LegacyHeartbeatReceived,
    ResearchLineReceived,
)
from pico_tui.research.stats import Summary, summarize, wilson

ECHO_TRAINS = 12           # trens guardados na sessão
LINK_WINDOWS = 240         # janelas de WIRELESS_STATS por sensor (5 s cada: 20 min)
HEARTBEAT_INTERVALS = 900  # intervalos do sinal de presença (15 min a 1 s)
BUS_WINDOWS = 900          # janelas de BUS_STATS (1 s)


def _int(fields: dict[str, str], key: str) -> int | None:
    try:
        return int(fields[key], 0)
    except (KeyError, ValueError):
        return None


def _float(fields: dict[str, str], key: str) -> float | None:
    try:
        return float(fields[key])
    except (KeyError, ValueError):
        return None


def _measured(fields: dict[str, str], key: str) -> float | None:
    """Valor em µs; -1 (sem amostra) vira None."""

    value = _float(fields, key)
    return None if value is None or value < 0 else value


@dataclass(slots=True)
class EchoProbe:
    index: int
    status: str
    rtt_us: float | None
    wl_us: float | None
    wall: float


@dataclass(slots=True)
class EchoTrain:
    node: int
    child: int
    path: str                     # LOCAL (só o CAN) ou SENSOR (até o sensor)
    count: int = 0
    period_ms: int = 0
    sampling: str = "PERIODIC"
    seed: int | None = None
    started_wall: float = field(default_factory=time.time)
    finished_wall: float | None = None
    probes: list[EchoProbe] = field(default_factory=list)
    done_sent: int | None = None
    done_received: int | None = None

    @property
    def target(self) -> str:
        return f"{self.node:02d}.{self.child:02d}"

    @property
    def active(self) -> bool:
        return self.finished_wall is None

    @property
    def sent(self) -> int:
        return self.done_sent if self.done_sent is not None else len(self.probes)

    @property
    def received(self) -> int:
        if self.done_received is not None:
            return self.done_received
        return sum(1 for probe in self.probes if probe.rtt_us is not None)

    @property
    def lost(self) -> int:
        return max(0, self.sent - self.received)

    def loss(self) -> tuple[float, float, float]:
        """Proporção de ecos sem resposta e IC 95 % de Wilson."""

        return wilson(self.lost, self.sent)

    def rtt(self) -> Summary:
        return summarize([probe.rtt_us for probe in self.probes if probe.rtt_us is not None])

    def wifi(self) -> Summary:
        """Parte Wi-Fi do eco SENSOR, medida pelo Node (relógio dele)."""

        return summarize([probe.wl_us for probe in self.probes if probe.rtt_us is not None and probe.wl_us is not None])

    def can_part(self) -> Summary:
        """Eco SENSOR menos a parte Wi-Fi: CAN e processamento no Node (intervalos de relógios diferentes)."""

        return summarize(
            [probe.rtt_us - probe.wl_us for probe in self.probes if probe.rtt_us is not None and probe.wl_us is not None]
        )

    def rtt_series(self) -> list[float]:
        return [probe.rtt_us for probe in self.probes if probe.rtt_us is not None]


@dataclass(slots=True)
class LinkStatsWindow:
    wall: float
    crypto_rx_us: float | None
    crypto_tx_us: float | None
    crypto_max_us: float | None
    handshake_us: float | None
    bridge_avg_us: float | None
    bridge_max_us: float | None


@dataclass(slots=True)
class LinkStats:
    logical_id: str
    windows: deque[LinkStatsWindow] = field(default_factory=lambda: deque(maxlen=LINK_WINDOWS))

    @property
    def latest(self) -> LinkStatsWindow | None:
        return self.windows[-1] if self.windows else None

    def summary(self, name: str) -> Summary:
        return summarize([getattr(window, name) for window in self.windows if getattr(window, name) is not None])

    def maximum(self, name: str) -> float | None:
        values = [getattr(window, name) for window in self.windows if getattr(window, name) is not None]
        return max(values) if values else None

    def last_handshake_us(self) -> float | None:
        for window in reversed(self.windows):
            if window.handshake_us is not None:
                return window.handshake_us
        return None


@dataclass(slots=True)
class BenchResult:
    domain: str
    op: str
    fields: dict[str, str]
    wall: float


@dataclass(slots=True)
class LoadRun:
    state: str
    fields: dict[str, object]
    wall: float


@dataclass(slots=True)
class ResearchSnapshot:
    now_wall: float
    config: dict[str, str] | None
    config_wall: float | None
    trains: list[EchoTrain]
    links: list[LinkStats]
    heartbeat_period_ms: int | None
    heartbeat_intervals_us: list[float]
    heartbeats_without_clock: int
    bus_loads: list[float]
    load: LoadRun | None
    bench: list[BenchResult]
    bench_cpu_mhz: int | None
    echo_error: str

    def latest_train(self, path: str) -> EchoTrain | None:
        for train in reversed(self.trains):
            if train.path == path:
                return train
        return None


class ResearchService:
    def __init__(self, bus: EventBus) -> None:
        self.bus = bus
        self.reset()
        for event_type, handler in (
            (ResearchLineReceived, self._on_line),
            (LegacyHeartbeatReceived, self._on_heartbeat),
            (BusStatsReceived, self._on_bus),
            (FaultEventReceived, self._on_fault),
            (ConnectionOpened, self._on_connection),
        ):
            bus.subscribe(event_type, handler)

    def reset(self) -> None:
        self.config: dict[str, str] | None = None
        self.config_wall: float | None = None
        self.trains: deque[EchoTrain] = deque(maxlen=ECHO_TRAINS)
        self.links: dict[str, LinkStats] = {}
        self.heartbeat_period_ms: int | None = None
        self.heartbeat_intervals: deque[float] = deque(maxlen=HEARTBEAT_INTERVALS)
        self.heartbeats_without_clock = 0
        self._last_heartbeat_us: int | None = None
        self._last_leader: int | None = None
        self.bus_loads: deque[float] = deque(maxlen=BUS_WINDOWS)
        self.load: LoadRun | None = None
        self.bench: dict[tuple[str, str], BenchResult] = {}
        self.bench_cpu_mhz: int | None = None
        self.echo_error = ""

    # ------------------------------------------------------------ eventos

    def _on_connection(self, _event: ConnectionOpened) -> None:
        self.reset()

    def _on_line(self, event: ResearchLineReceived) -> None:
        fields = event.fields
        kind = event.kind
        now = time.time()
        if kind == "CONFIG":
            self.config = dict(fields)
            self.config_wall = now
        elif kind == "ECHO_START":
            self.trains.append(
                EchoTrain(
                    node=_int(fields, "NODE") or 0,
                    child=_int(fields, "CHILD") or 0,
                    path=fields.get("PATH", "LOCAL").upper(),
                    count=_int(fields, "COUNT") or 0,
                    period_ms=_int(fields, "PERIOD_MS") or 0,
                    sampling=fields.get("SAMPLING", "PERIODIC").upper(),
                    seed=_int(fields, "SEED"),
                    started_wall=now,
                )
            )
            self.echo_error = ""
        elif kind == "ECHO":
            train = self._train_for(fields)
            if train is None:
                return
            status = fields.get("STATUS", "").upper()
            rtt = _measured(fields, "RTT_US") if status == "APPLIED" else None
            train.probes.append(
                EchoProbe(
                    index=_int(fields, "N") or len(train.probes) + 1,
                    status=status,
                    rtt_us=rtt,
                    wl_us=_measured(fields, "WL_US"),
                    wall=now,
                )
            )
        elif kind == "ECHO_DONE":
            train = self._train_for(fields)
            if train is None:
                return
            train.done_sent = _int(fields, "SENT")
            train.done_received = _int(fields, "RECEIVED")
            train.finished_wall = now
        elif kind == "ECHO_ERROR":
            self.echo_error = event.raw.split("ECHO_ERROR", 1)[-1].strip()
        elif kind == "WIRELESS_STATS":
            node, child = _int(fields, "NODE"), _int(fields, "CHILD")
            if node is None or child is None:
                return
            logical_id = f"{node:02d}.{child:02d}"
            entry = self.links.setdefault(logical_id, LinkStats(logical_id))
            entry.windows.append(
                LinkStatsWindow(
                    wall=now,
                    crypto_rx_us=_measured(fields, "CRYPTO_RX_US"),
                    crypto_tx_us=_measured(fields, "CRYPTO_TX_US"),
                    crypto_max_us=_measured(fields, "CRYPTO_MAX_US"),
                    handshake_us=_measured(fields, "HANDSHAKE_US"),
                    bridge_avg_us=_measured(fields, "BRIDGE_AVG_US"),
                    bridge_max_us=_measured(fields, "BRIDGE_MAX_US"),
                )
            )
        elif kind == "BENCH":
            domain, op = fields.get("DOMAIN", "?"), fields.get("OP", "?")
            self.bench[(domain, op)] = BenchResult(domain, op, dict(fields), now)
        elif kind == "BENCH_DONE":
            self.bench_cpu_mhz = _int(fields, "CPU_MHZ")

    def _train_for(self, fields: dict[str, str]) -> EchoTrain | None:
        node, child = _int(fields, "NODE"), _int(fields, "CHILD")
        path = fields.get("PATH", "").upper()
        for train in reversed(self.trains):
            if train.node == node and train.child == child and train.path == path and train.active:
                return train
        return None

    def _on_heartbeat(self, event: LegacyHeartbeatReceived) -> None:
        self.heartbeat_period_ms = event.period_ms or self.heartbeat_period_ms
        if event.t_us is None:
            self.heartbeats_without_clock += 1
            return
        # Uma troca de líder recomeça a série: o intervalo entre o último sinal
        # do antigo e o primeiro do novo mede a eleição, não a variação.
        if self._last_leader is not None and event.leader != self._last_leader:
            self._last_heartbeat_us = None
        self._last_leader = event.leader
        if self._last_heartbeat_us is not None and event.t_us > self._last_heartbeat_us:
            self.heartbeat_intervals.append(float(event.t_us - self._last_heartbeat_us))
        self._last_heartbeat_us = event.t_us

    def _on_bus(self, event: BusStatsReceived) -> None:
        self.bus_loads.append(event.load_percent)

    def _on_fault(self, event: FaultEventReceived) -> None:
        if event.kind.upper() != "BUS_LOAD":
            return
        self.load = LoadRun(event.state.upper(), dict(event.payload), time.time())

    # ------------------------------------------------------------ leitura

    def snapshot(self) -> ResearchSnapshot:
        return ResearchSnapshot(
            now_wall=time.time(),
            config=dict(self.config) if self.config is not None else None,
            config_wall=self.config_wall,
            trains=list(self.trains),
            links=[self.links[key] for key in sorted(self.links)],
            heartbeat_period_ms=self.heartbeat_period_ms,
            heartbeat_intervals_us=list(self.heartbeat_intervals),
            heartbeats_without_clock=self.heartbeats_without_clock,
            bus_loads=list(self.bus_loads),
            load=self.load,
            bench=sorted(self.bench.values(), key=lambda item: (item.domain, item.op)),
            bench_cpu_mhz=self.bench_cpu_mhz,
            echo_error=self.echo_error,
        )
