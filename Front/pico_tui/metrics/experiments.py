"""Ensaios de injeção de falhas: catálogo e medição dos resultados.

O catálogo descreve cada ensaio em linguagem de operador e sabe montar o
comando da Probe 00. O registrador acompanha um ensaio em curso a partir dos
eventos de protocolo e mede, sem intervenção, o que interessa em cada caso:
tempo até a rede ter líder de novo, tempo sem telemetria, reassociações,
ocupação máxima do barramento.

Um ensaio também é reconhecido quando começa fora da tela (comando manual,
reprodução de uma gravação): a linha FAULT_TX ou FAULT_EVENT o inicia.
"""
from __future__ import annotations

import time
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass, field

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    BusStatsReceived,
    FaultCommandSent,
    FaultEventReceived,
    IdStatsReceived,
    LegacyHeartbeatReceived,
    NetworkEventReceived,
    PhysicalNodeReceived,
    TelemetryReceived,
    WirelessFailoverReceived,
    WirelessLinkReceived,
)
from pico_tui.metrics.catalog import HEARTBEAT_ID

Clock = Callable[[], float]

MAX_SECONDS = 120
MANUAL_MAX_SECONDS = 600
RUN_HISTORY = 20
# Um sensor é considerado ativo no início do ensaio se enviou neste intervalo.
TELEMETRY_RECENT_SECONDS = 10.0
# Silêncio, ao final do ensaio, a partir do qual a telemetria é dada como não retomada.
TELEMETRY_NOT_RESUMED_SECONDS = 5.0


@dataclass(frozen=True, slots=True)
class Experiment:
    key: str
    title: str
    summary: str
    kind: str                       # tipo no firmware, ou LOAD / MANUAL
    target: str = "node"            # node, leader, owner ou none
    default_seconds: int = 20
    min_seconds: int = 5
    max_seconds: int = MAX_SECONDS
    settle_seconds: int = 20        # observação após o fim da falha
    measures: tuple[str, ...] = ()
    effect: str = ""                # o que acontece com o equipamento
    after: str = ""                 # o que o operador precisa fazer depois
    load_priority: str = ""         # HIGH ou LOW, para os ensaios de carga

    def command(self, node_id: int | None, seconds: int, load_percent: int = 40) -> str | None:
        if self.kind == "MANUAL":
            return None
        if self.kind == "LOAD":
            return f"FAULT LOAD {load_percent} {seconds} {self.load_priority or 'HIGH'}"
        if self.kind == "DROP_SESSION":
            return f"FAULT DROP_SESSION {node_id}"
        return f"FAULT {self.kind} {node_id} {seconds}"


_COORDINATION = ("leader_detect_s", "leader_recovered_s", "heartbeat_gap_max_s", "elections", "leader_after")
_WIRELESS = ("failover_decision_s", "failovers", "session_recovered_s", "telemetry_gap_max_s", "telemetry_lost")
_BUS = ("load_max", "peak_max", "heartbeat_jitter_max_ms", "tx_err_max", "rx_err_max", "transfer_errors")

EXPERIMENTS: tuple[Experiment, ...] = (
    Experiment(
        key="leader_power_cycle",
        title="Queda do líder",
        summary="O líder sai do ar e reinicia. Mede quanto tempo a rede fica sem coordenação "
                "e quem assume a liderança.",
        kind="POWER_CYCLE",
        target="leader",
        default_seconds=20,
        measures=(*_COORDINATION, "node_rejoined_s", "nodes_in_fault", *_WIRELESS),
        effect="O módulo deixa de transmitir e de confirmar quadros, desliga os rádios e reinicia ao final. "
               "Perde os vínculos com sensores sem fio, que ficam em memória.",
        after="O módulo volta pedindo entrada na rede e permanece seguidor até a próxima eleição.",
    ),
    Experiment(
        key="node_power_cycle",
        title="Queda de um módulo",
        summary="Um módulo seguidor sai do ar e reinicia. Mede a detecção da ausência e, se ele tinha "
                "sensores sem fio com reassociação automática, o tempo até os dados voltarem por outro módulo.",
        kind="POWER_CYCLE",
        target="owner",
        default_seconds=40,
        settle_seconds=30,
        measures=("node_fault_s", "node_rejoined_s", "nodes_in_fault", *_WIRELESS, "heartbeat_gap_max_s"),
        effect="O módulo deixa de transmitir, desliga os rádios e reinicia ao final.",
        after="O líder marca o módulo ausente como falho; a falha permanece registrada até o comando "
              "Limpar falha (tela do módulo, Comandos).",
    ),
    Experiment(
        key="node_silence",
        title="Módulo congelado",
        summary="O módulo para de executar por um tempo e retoma com o estado que tinha, como em um "
                "travamento. Aplicado ao líder, mostra como a rede resolve dois líderes ao mesmo tempo.",
        kind="SILENCE",
        target="node",
        default_seconds=15,
        measures=(*_COORDINATION, "nodes_in_fault", "leaders_seen", *_WIRELESS),
        effect="O módulo não transmite nem processa mensagens; os rádios continuam ligados. Não reinicia.",
        after="Verifique se a rede terminou com um único líder.",
    ),
    Experiment(
        key="ap_outage",
        title="Queda do ponto de acesso",
        summary="O módulo desliga o Wi-Fi por alguns segundos. Mede quanto tempo o sensor leva para "
                "perceber, voltar ao ponto de acesso e se autenticar de novo.",
        kind="AP_OUTAGE",
        target="owner",
        default_seconds=8,
        settle_seconds=25,
        measures=("session_down_s", "session_recovered_s", "telemetry_gap_max_s", "telemetry_lost", "failovers"),
        effect="As sessões com os sensores do módulo são encerradas; o barramento CAN não é afetado.",
    ),
    Experiment(
        key="drop_session",
        title="Sessões descartadas",
        summary="O módulo esquece as chaves de sessão sem avisar o sensor. Mede a detecção pelo sensor "
                "(silêncio do enlace) e a nova autenticação.",
        kind="DROP_SESSION",
        target="owner",
        default_seconds=0,
        min_seconds=0,
        settle_seconds=30,
        measures=("session_down_s", "session_recovered_s", "telemetry_gap_max_s", "telemetry_lost", "failovers"),
        effect="O ponto de acesso continua ligado; os datagramas do sensor passam a ser recusados.",
    ),
    Experiment(
        key="bus_load_high",
        title="Carga de alta prioridade",
        summary="A Probe 00 ocupa o barramento com quadros que vencem a arbitragem contra todo o "
                "tráfego do projeto. Mede o atraso imposto ao sinal de presença e à telemetria.",
        kind="LOAD",
        target="none",
        default_seconds=15,
        settle_seconds=8,
        measures=(*_BUS, "heartbeat_gap_max_s", "telemetry_gap_max_s", "telemetry_lost", "elections"),
        effect="Os módulos ignoram os quadros de carga; apenas disputam o barramento com eles.",
        load_priority="HIGH",
    ),
    Experiment(
        key="bus_load_low",
        title="Carga de baixa prioridade",
        summary="A Probe 00 ocupa o barramento com quadros que perdem a arbitragem para todo o tráfego "
                "do projeto. O efeito esperado é apenas o bloqueio de um quadro por vez.",
        kind="LOAD",
        target="none",
        default_seconds=15,
        settle_seconds=8,
        measures=(*_BUS, "heartbeat_gap_max_s", "telemetry_gap_max_s", "telemetry_lost", "elections"),
        effect="Os módulos ignoram os quadros de carga.",
        load_priority="LOW",
    ),
    Experiment(
        key="manual",
        title="Ensaio manual",
        summary="Marca o início e o fim de uma falha provocada à mão (desligar a alimentação de um "
                "módulo, desconectar o barramento) e mede os mesmos tempos.",
        kind="MANUAL",
        target="none",
        default_seconds=MANUAL_MAX_SECONDS,
        max_seconds=MANUAL_MAX_SECONDS,
        settle_seconds=0,
        measures=(*_COORDINATION, "nodes_in_fault", *_WIRELESS, "load_max"),
        effect="Nenhum comando é enviado. Provoque a falha logo após iniciar e encerre com s.",
    ),
)

EXPERIMENTS_BY_KEY = {experiment.key: experiment for experiment in EXPERIMENTS}

# (rótulo, unidade, casas decimais). Unidade "" = contagem ou texto.
MEASURE_LABELS: dict[str, tuple[str, str, int]] = {
    "leader_detect_s": ("Falta do líder percebida em", "s", 2),
    "leader_recovered_s": ("Rede com líder novamente em", "s", 2),
    "heartbeat_gap_max_s": ("Maior intervalo sem sinal de presença", "s", 2),
    "elections": ("Eleições solicitadas", "", 0),
    "leader_after": ("Líder ao final", "", 0),
    "leaders_seen": ("Líderes observados durante o ensaio", "", 0),
    "node_fault_s": ("Módulo declarado ausente em", "s", 2),
    "node_rejoined_s": ("Módulo pediu para voltar à rede em", "s", 2),
    "nodes_in_fault": ("Módulos marcados em falha", "", 0),
    "failover_decision_s": ("Reassociação decidida em", "s", 2),
    "failovers": ("Reassociações decididas", "", 0),
    "session_down_s": ("Sessão encerrada em", "s", 2),
    "session_recovered_s": ("Sessão autenticada novamente em", "s", 2),
    "telemetry_gap_max_s": ("Maior intervalo sem telemetria", "s", 2),
    "telemetry_lost": ("Amostras de telemetria perdidas", "", 0),
    "load_max": ("Ocupação máxima do barramento (1 s)", "%", 2),
    "peak_max": ("Pico de ocupação (100 ms)", "%", 2),
    "heartbeat_jitter_max_ms": ("Maior variação do período do sinal de presença", "ms", 2),
    "tx_err_max": ("Contador de erros de transmissão (máx.)", "", 0),
    "rx_err_max": ("Contador de erros de recepção (máx.)", "", 0),
    "transfer_errors": ("Transferências segmentadas descartadas", "", 0),
}

KIND_TO_EXPERIMENT = {
    "POWER_CYCLE": "node_power_cycle",
    "SILENCE": "node_silence",
    "AP_OUTAGE": "ap_outage",
    "DROP_SESSION": "drop_session",
}


@dataclass(slots=True)
class RunEvent:
    t: float       # segundos desde o início do ensaio
    text: str


@dataclass(slots=True)
class ExperimentRun:
    experiment: Experiment
    node_id: int | None
    seconds: int
    load_percent: int | None = None
    command: str | None = None
    started_wall: float = field(default_factory=time.time)
    t0: float = 0.0
    state: str = "RUNNING"          # RUNNING, DONE ou STOPPED
    finished_after_s: float | None = None
    events: list[RunEvent] = field(default_factory=list)
    metrics: dict[str, object] = field(default_factory=dict)
    leader_before: int | None = None
    origin: str = "TUI"             # TUI ou OBSERVED (iniciado fora desta tela)

    # Estado interno da medição.
    _fault_started: float | None = None
    _fault_ended: float | None = None
    _last_heartbeat: float | None = None
    _leaders: set[int] = field(default_factory=set)
    _coordination_lost: bool = False
    _telemetry_last: dict[str, float] = field(default_factory=dict)
    _telemetry_seq: dict[str, int] = field(default_factory=dict)
    _link_state: dict[str, str] = field(default_factory=dict)
    _transfer_errors_start: int | None = None
    _faults: set[int] = field(default_factory=set)
    _fault_seq: int | None = None

    @property
    def title(self) -> str:
        target = ""
        if self.node_id:
            target = f", Módulo {self.node_id:02d}"
        elif self.load_percent:
            target = f", {self.load_percent} %"
        duration = f", {self.seconds} s" if self.seconds and self.experiment.kind != "MANUAL" else ""
        return f"{self.experiment.title}{target}{duration}"

    @property
    def planned_seconds(self) -> float:
        return float(self.seconds + self.experiment.settle_seconds)

    def rows(self) -> list[tuple[str, str]]:
        """Resultados na ordem do catálogo, já formatados."""

        rows: list[tuple[str, str]] = []
        for key in self.experiment.measures:
            label, unit, decimals = MEASURE_LABELS[key]
            value = self.metrics.get(key)
            if key == "leader_after":
                text = f"Módulo {value:02d}" if isinstance(value, int) and value else "nenhum"
            else:
                text = format_measure(value, unit, decimals)
            rows.append((label, text))
        return rows


def format_measure(value: object, unit: str, decimals: int) -> str:
    if value is None:
        return "não observado"
    if isinstance(value, (set, frozenset, list, tuple)):
        if not value:
            return "nenhum"
        return ", ".join(f"{item:02d}" if isinstance(item, int) else str(item) for item in sorted(value))
    if isinstance(value, bool):
        return "sim" if value else "não"
    if isinstance(value, float):
        text = f"{value:.{decimals}f}".replace(".", ",")
        return f"{text} {unit}".strip()
    if isinstance(value, int):
        return f"{value} {unit}".strip()
    return str(value)


class ExperimentRecorder:
    def __init__(self, bus: EventBus, clock: Clock = time.monotonic) -> None:
        self.bus = bus
        self.clock = clock
        self.active: ExperimentRun | None = None
        self.history: deque[ExperimentRun] = deque(maxlen=RUN_HISTORY)
        self._leader: int | None = None
        self._last_heartbeat: float | None = None
        self._links: dict[str, str] = {}      # último estado conhecido de cada enlace
        # Última amostra de cada sensor, acompanhada também fora dos ensaios:
        # o intervalo sem telemetria conta a partir da última amostra recebida
        # antes da falha, qualquer que seja a ordem em que as linhas chegam.
        self._telemetry_last: dict[str, float] = {}
        self._telemetry_seq: dict[str, int] = {}
        self._expecting: tuple[str, int | None] | None = None
        for event_type, handler in (
            (FaultCommandSent, self._on_command),
            (FaultEventReceived, self._on_fault),
            (LegacyHeartbeatReceived, self._on_heartbeat),
            (NetworkEventReceived, self._on_network_event),
            (PhysicalNodeReceived, self._on_node),
            (WirelessFailoverReceived, self._on_failover),
            (WirelessLinkReceived, self._on_link),
            (TelemetryReceived, self._on_telemetry),
            (BusStatsReceived, self._on_bus),
            (IdStatsReceived, self._on_id),
        ):
            bus.subscribe(event_type, handler)

    # ------------------------------------------------------------ controle

    def reset(self) -> None:
        """Esquece o que foi observado até aqui (nova conexão ou nova gravação)."""

        self.active = None
        self._leader = None
        self._last_heartbeat = None
        self._links.clear()
        self._telemetry_last.clear()
        self._telemetry_seq.clear()
        self._expecting = None

    @property
    def current_leader(self) -> int | None:
        return self._leader

    def start(
        self,
        experiment: Experiment,
        node_id: int | None,
        seconds: int,
        *,
        load_percent: int | None = None,
        origin: str = "TUI",
    ) -> ExperimentRun:
        if self.active is not None:
            self.stop()
        run = ExperimentRun(
            experiment=experiment,
            node_id=node_id,
            seconds=seconds,
            load_percent=load_percent,
            command=experiment.command(node_id, seconds, load_percent or 40),
            t0=self.clock(),
            leader_before=self._leader,
            origin=origin,
        )
        # O intervalo sem sinal de presença conta a partir do último sinal
        # recebido antes do ensaio.
        run._last_heartbeat = self._last_heartbeat
        run._link_state = dict(self._links)
        # Só os sensores que vinham enviando: um identificador que deixou de
        # existir (sensor removido ou já reassociado) não entra na medição.
        run._telemetry_last = {
            key: last for key, last in self._telemetry_last.items() if run.t0 - last <= TELEMETRY_RECENT_SECONDS
        }
        run._telemetry_seq = {key: self._telemetry_seq[key] for key in run._telemetry_last if key in self._telemetry_seq}
        if self._leader:
            run._leaders.add(self._leader)
        self.active = run
        self._expecting = (experiment.kind, node_id) if origin == "TUI" else None
        self._event(run, "Ensaio iniciado" + (f": {run.command}" if run.command else ""))
        if experiment.kind == "MANUAL":
            run._fault_started = run.t0
        return run

    def stop(self) -> ExperimentRun | None:
        """Encerra o ensaio em curso antes do prazo (decisão do operador)."""

        return self._finish("STOPPED")

    def poll(self) -> ExperimentRun | None:
        """Chamado periodicamente; encerra o ensaio quando o prazo se cumpre."""

        run = self.active
        if run is None:
            return None
        elapsed = self.clock() - run.t0
        limit = run.planned_seconds if run.experiment.kind != "MANUAL" else float(MANUAL_MAX_SECONDS)
        if elapsed >= limit:
            return self._finish("DONE")
        return None

    def elapsed(self) -> float:
        return self.clock() - self.active.t0 if self.active else 0.0

    def _finish(self, state: str) -> ExperimentRun | None:
        run = self.active
        if run is None:
            return None
        run.state = state
        run.finished_after_s = self.clock() - run.t0
        metrics = run.metrics
        metrics.setdefault("elections", 0)
        metrics.setdefault("failovers", 0)
        metrics.setdefault("telemetry_lost", 0)
        # Um sensor que não voltou a enviar conta até o fim do ensaio.
        ended = self.clock()
        for logical_id, last in run._telemetry_last.items():
            gap = ended - last
            if gap > TELEMETRY_NOT_RESUMED_SECONDS:
                self._maximum(run, "telemetry_gap_max_s", gap)
                self._event(run, f"Sensor {logical_id}: telemetria não retomada até o fim do ensaio")
            elif "telemetry_gap_max_s" in metrics:
                self._maximum(run, "telemetry_gap_max_s", gap)
        metrics["nodes_in_fault"] = set(run._faults)
        metrics["leader_after"] = self._leader
        metrics["leaders_seen"] = set(run._leaders)
        self._event(run, "Ensaio encerrado" if state == "DONE" else "Ensaio encerrado pelo operador")
        self.history.appendleft(run)
        self.active = None
        self._expecting = None
        return run

    # ------------------------------------------------------------ medição

    def _event(self, run: ExperimentRun, text: str) -> None:
        run.events.append(RunEvent(self.clock() - run.t0, text))

    def _since_fault(self, run: ExperimentRun) -> float:
        origin = run._fault_started if run._fault_started is not None else run.t0
        return self.clock() - origin

    def _first(self, run: ExperimentRun, key: str, value: object) -> bool:
        if key in run.metrics:
            return False
        run.metrics[key] = value
        return True

    def _maximum(self, run: ExperimentRun, key: str, value: float) -> None:
        previous = run.metrics.get(key)
        if previous is None or value > previous:  # type: ignore[operator]
            run.metrics[key] = value

    def _on_command(self, event: FaultCommandSent) -> None:
        if event.kind == "CANCEL":
            return
        expected = self._expecting
        if self.active is not None and expected is not None and expected[0] == event.kind:
            self._expecting = None
            return
        if not self._make_room():
            return
        self._start_observed(event.kind, event.node_id, event.duration_ms)

    def _make_room(self) -> bool:
        """Outro ensaio começou fora desta tela: o anterior, se a falha dele já
        terminou e só restava a observação, é encerrado para dar lugar."""

        run = self.active
        if run is None:
            return True
        if run._fault_ended is None:
            return False
        self._finish("DONE")
        return True

    def _start_observed(self, kind: str, node_id: int, duration_ms: int, load_percent: int | None = None,
                        priority: str = "HIGH") -> None:
        if kind == "BUS_LOAD":
            key = "bus_load_low" if priority == "LOW" else "bus_load_high"
        elif kind == "POWER_CYCLE" and node_id and node_id == self._leader:
            key = "leader_power_cycle"
        else:
            key = KIND_TO_EXPERIMENT.get(kind, "")
        experiment = EXPERIMENTS_BY_KEY.get(key)
        if experiment is None:
            return
        self.start(experiment, node_id or None, max(0, duration_ms // 1000), load_percent=load_percent, origin="OBSERVED")

    def _on_fault(self, event: FaultEventReceived) -> None:
        run = self.active
        if (
            run is not None
            and event.state == "STARTED"
            and run._fault_seq is not None
            and event.seq != run._fault_seq
            and self._make_room()
        ):
            run = None
        if run is None:
            if event.state == "STARTED":
                percent = None
                try:
                    percent = int(str(event.payload.get("LOAD_PCT"))) if event.payload.get("LOAD_PCT") else None
                except ValueError:
                    percent = None
                self._start_observed(event.kind, event.node_id, event.duration_ms, percent,
                                     str(event.payload.get("PRIORITY", "HIGH")).upper())
                run = self.active
            if run is None:
                return
        now = self.clock()
        where = f"Módulo {event.node_id:02d}" if event.node_id else "Probe 00"
        if event.state == "STARTED":
            if run._fault_started is None:
                run._fault_started = now
            if run._fault_seq is None:
                run._fault_seq = event.seq
            self._event(run, f"{where}: falha iniciada")
        elif event.state == "ENDED":
            run._fault_ended = now
            self._event(run, f"{where}: falha encerrada")
            if event.kind == "BUS_LOAD":
                achieved = event.payload.get("ACHIEVED_PCT")
                if achieved is not None:
                    self._event(run, f"Carga efetivamente gerada: {achieved} %")
        elif event.state == "REJECTED":
            self._event(run, f"{where}: ensaio recusado (outro ensaio em curso ou tipo não suportado)")

    def _on_heartbeat(self, event: LegacyHeartbeatReceived) -> None:
        if event.leader:
            self._leader = event.leader
        now = self.clock()
        self._last_heartbeat = now
        run = self.active
        if run is None:
            return
        if run._last_heartbeat is not None:
            self._maximum(run, "heartbeat_gap_max_s", now - run._last_heartbeat)
        run._last_heartbeat = now
        if event.leader:
            if event.leader not in run._leaders:
                run._leaders.add(event.leader)
                self._event(run, f"Sinal de presença do líder Módulo {event.leader:02d}")
            # A rede "voltou" no primeiro sinal de presença depois de a
            # coordenação ter sido dada como perdida.
            if run._coordination_lost and self._first(run, "leader_recovered_s", self._since_fault(run)):
                self._event(run, f"Rede com líder: Módulo {event.leader:02d}")

    def _on_network_event(self, event: NetworkEventReceived) -> None:
        run = self.active
        if run is None:
            return
        if event.kind == "LEADER_FAILURE_DETECTED":
            run._coordination_lost = True
            if self._first(run, "leader_detect_s", self._since_fault(run)):
                self._event(run, f"Probe 00 percebeu a falta do líder Módulo {event.node_id:02d}")
        elif event.kind == "ELECTION_REQUESTED":
            run._coordination_lost = True
            run.metrics["elections"] = int(run.metrics.get("elections", 0)) + 1  # type: ignore[arg-type]
            self._first(run, "leader_detect_s", self._since_fault(run))
            self._event(run, "Eleição solicitada")
        elif event.kind == "LEADER_ANNOUNCED":
            self._event(run, f"Módulo {event.node_id:02d} anunciou a liderança")
        elif event.kind == "JOIN_OBSERVED":
            if run.node_id is None or event.node_id == run.node_id:
                if self._first(run, "node_rejoined_s", self._since_fault(run)):
                    self._event(run, f"Módulo {event.node_id:02d} pediu para voltar à rede")

    def _on_node(self, event: PhysicalNodeReceived) -> None:
        run = self.active
        if run is None or event.parent_node_id == 0:
            return
        if str(event.payload.get("STATE", "")).upper() != "FALHA":
            return
        if event.parent_node_id in run._faults:
            return
        run._faults.add(event.parent_node_id)
        self._event(run, f"Módulo {event.parent_node_id:02d} marcado em falha pelo líder")
        if run.node_id is None or event.parent_node_id == run.node_id:
            self._first(run, "node_fault_s", self._since_fault(run))

    def _on_failover(self, event: WirelessFailoverReceived) -> None:
        run = self.active
        if run is None:
            return
        run.metrics["failovers"] = int(run.metrics.get("failovers", 0)) + 1  # type: ignore[arg-type]
        self._first(run, "failover_decision_s", self._since_fault(run))
        self._event(
            run,
            f"Sensor {event.wireless_uuid} reassociado: Módulo {event.from_node_id:02d} → Módulo {event.to_node_id:02d}",
        )

    def _on_link(self, event: WirelessLinkReceived) -> None:
        run = self.active
        logical_id = f"{event.parent_node_id:02d}.{event.child_id:02d}"
        state = event.state.upper()
        self._links[logical_id] = state
        if run is None:
            return
        previous = run._link_state.get(logical_id)
        run._link_state[logical_id] = state
        if previous is None:
            # Primeiro relato deste enlace: só conta como retomada se a sessão
            # anterior caiu ou o sensor foi reassociado neste ensaio.
            if state == "SECURE" and ("session_down_s" in run.metrics or "failover_decision_s" in run.metrics):
                if self._first(run, "session_recovered_s", self._since_fault(run)):
                    self._event(run, f"Sensor {logical_id}: sessão autenticada")
            return
        if previous == "SECURE" and state != "SECURE":
            if self._first(run, "session_down_s", self._since_fault(run)):
                self._event(run, f"Sensor {logical_id}: sessão encerrada")
        elif previous != "SECURE" and state == "SECURE":
            if self._first(run, "session_recovered_s", self._since_fault(run)):
                self._event(run, f"Sensor {logical_id}: sessão autenticada novamente")

    def _on_telemetry(self, event: TelemetryReceived) -> None:
        sample = event.sample
        logical_id = f"{sample.parent_node_id:02d}.{sample.child_id:02d}"
        now = self.clock()
        run = self.active
        if run is None:
            self._telemetry_last[logical_id] = now
            if sample.sequence is not None:
                self._telemetry_seq[logical_id] = sample.sequence
            return
        if logical_id in run._telemetry_last:
            self._maximum(run, "telemetry_gap_max_s", now - run._telemetry_last[logical_id])
        else:
            # Identificador novo durante o ensaio: depois de uma reassociação
            # o sensor passa a chegar por outro módulo. Ele continua o
            # histórico do sensor que estava calado há mais tempo.
            silent = {key: last for key, last in run._telemetry_last.items() if now - last > 3.0}
            if silent:
                previous_id = min(silent, key=silent.get)  # type: ignore[arg-type]
                self._maximum(run, "telemetry_gap_max_s", now - silent[previous_id])
                del run._telemetry_last[previous_id]
                run._telemetry_seq.pop(previous_id, None)
                self._telemetry_last.pop(previous_id, None)
                self._telemetry_seq.pop(previous_id, None)
                self._event(run, f"Telemetria do sensor {previous_id} passou a chegar como {logical_id}")
                if "failover_decision_s" in run.metrics:
                    self._first(run, "session_recovered_s", self._since_fault(run))
        run._telemetry_last[logical_id] = now
        if sample.sequence is not None:
            previous = run._telemetry_seq.get(logical_id)
            if previous is not None:
                step = (sample.sequence - previous) & 0xFFFF
                if 1 < step < 0x8000:
                    run.metrics["telemetry_lost"] = int(run.metrics.get("telemetry_lost", 0)) + step - 1  # type: ignore[arg-type]
            run._telemetry_seq[logical_id] = sample.sequence
        self._telemetry_last[logical_id] = now
        if sample.sequence is not None:
            self._telemetry_seq[logical_id] = sample.sequence

    def _on_bus(self, event: BusStatsReceived) -> None:
        run = self.active
        if run is None:
            return
        self._maximum(run, "load_max", event.load_percent)
        self._maximum(run, "peak_max", event.peak_percent)
        self._maximum(run, "tx_err_max", event.tx_errors)
        self._maximum(run, "rx_err_max", event.rx_errors)
        if run._transfer_errors_start is None:
            run._transfer_errors_start = event.transfers_err
        run.metrics["transfer_errors"] = event.transfers_err - run._transfer_errors_start

    def _on_id(self, event: IdStatsReceived) -> None:
        run = self.active
        if run is None or event.can_id != HEARTBEAT_ID or event.count < 2:
            return
        self._maximum(run, "heartbeat_jitter_max_ms", (event.dt_max_us - event.dt_min_us) / 1000.0)
