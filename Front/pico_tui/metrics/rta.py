"""Análise de tempo de resposta das mensagens CAN observadas.

Segue a análise de escalonabilidade do CAN revisada por Davis, Burns, Bril e
Lukkien ("Controller Area Network (CAN) schedulability analysis: Refuted,
revisited and revised", Real-Time Systems, 2007), que corrige a de Tindell,
Burns e Wellings (1995).

Cada identificador é um fluxo com prioridade fixa (menor identificador vence
a arbitragem). O que diferencia este módulo da formulação clássica é a origem
dos parâmetros: período, variação e tamanho de rajada vêm do que a Probe 00
mediu no barramento (linhas ID_STATS), não de uma tabela de projeto.

Modelo de um fluxo:

    C   duração de um quadro no pior caso de bits de preenchimento
    P   período entre rajadas
    b   quadros por rajada (1 para mensagens de um quadro; 9 para uma amostra
        de telemetria segmentada, por exemplo)
    J   variação da liberação (zero para os fluxos medidos: o período usado
        já é o menor intervalo observado entre chegadas)
    D   prazo (por padrão, o próprio período)

Para a mensagem m, com hp(m) o conjunto de maior prioridade:

    B_m   = max C_k entre as de menor prioridade (um quadro já em transmissão)
    t_m   = B_m + soma_{k em hp(m) ou m} b_k * ceil((t_m + J_k) / P_k) * C_k
    Q_m   = b_m * ceil((t_m + J_m) / P_m)
    w_m(q)= B_m + q*C_m + soma_{k em hp(m)} b_k * ceil((w_m(q) + J_k + tau) / P_k) * C_k
    R_m(q)= J_m + w_m(q) - floor(q / b_m) * P_m + C_m
    R_m   = max_q R_m(q)

Com b = 1, as expressões são as do artigo. A extensão para
rajadas trata os b quadros como liberados juntos a cada período.

Limites do modelo, que valem como ressalva em qualquer conclusão:
  * os parâmetros são os observados na janela de medição, não o pior caso
    de projeto;
  * o tamanho das rajadas é inferido de três números por identificador
    (intervalo mínimo, médio e máximo), não contado;
  * o instante crítico supõe todas as mensagens liberadas juntas, o que é
    pessimista para tráfego sincronizado pelo sinal de presença do líder;
  * o controlador MCP2515 é usado com uma fila de transmissão em ordem de
    chegada: dentro de um mesmo módulo, um quadro de menor prioridade pode
    atrasar outro de maior prioridade, efeito que a análise não inclui;
  * erros de transmissão e retransmissões não são considerados.
"""
from __future__ import annotations

import math
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, replace

DEFAULT_BITRATE = 500_000
# Uma amostra de telemetria (50 bytes) ocupa 1 + ceil(46 / 6) = 9 quadros.
TELEMETRY_FRAMES = 9
MAX_INSTANCES = 4096
MAX_BURST = 64
MAX_ITERATIONS = 10_000


def frame_bits_worst(dlc: int) -> int:
    """Quadro de dados com identificador de 11 bits, no pior caso de preenchimento."""

    dlc = max(0, min(8, int(dlc)))
    stuffable = 34 + 8 * dlc
    return stuffable + 13 + (stuffable - 1) // 4


@dataclass(frozen=True, slots=True)
class Flow:
    can_id: int
    name: str
    dlc: int
    period_us: float
    burst: int = 1
    jitter_us: float = 0.0
    deadline_us: float | None = None
    observed_frames: int = 0
    synthetic: bool = False
    # Falso para tráfego sem requisito de tempo (carga artificial de ensaio):
    # interfere nos demais, mas não tem prazo a cumprir.
    has_deadline: bool = True
    # Verdadeiro quando o tamanho da rajada foi inferido dos intervalos
    # medidos, e não contado.
    estimated: bool = False

    @property
    def deadline(self) -> float:
        return self.deadline_us if self.deadline_us is not None else self.period_us


@dataclass(frozen=True, slots=True)
class FlowResult:
    flow: Flow
    frame_us: float
    utilization: float
    blocking_us: float
    response_first_us: float | None   # primeiro quadro da rajada
    response_us: float | None         # pior quadro; None quando não converge
    deadline_us: float

    @property
    def schedulable(self) -> bool:
        if not self.flow.has_deadline:
            return True
        return self.response_us is not None and self.response_us <= self.deadline_us

    @property
    def slack_percent(self) -> float | None:
        if not self.flow.has_deadline:
            return None
        if self.response_us is None or self.deadline_us <= 0:
            return -100.0
        return 100.0 * (self.deadline_us - self.response_us) / self.deadline_us


@dataclass(frozen=True, slots=True)
class Analysis:
    results: tuple[FlowResult, ...]
    bitrate: int
    utilization: float

    @property
    def schedulable(self) -> bool:
        return bool(self.results) and all(result.schedulable for result in self.results)

    @property
    def critical(self) -> FlowResult | None:
        """Fluxo com a menor folga (ou o primeiro que não cumpre o prazo)."""

        candidates = [result for result in self.results if result.flow.has_deadline]
        if not candidates:
            return None
        return min(candidates, key=lambda result: result.slack_percent if result.slack_percent is not None else 0.0)


def flow_from_observation(
    can_id: int,
    name: str,
    *,
    count: int,
    window_ms: int,
    dlc: int,
    dt_min_us: int,
    dt_avg_us: int,
    dt_max_us: int,
    bitrate: int = DEFAULT_BITRATE,
    has_deadline: bool = True,
) -> Flow | None:
    """Estima período e rajada a partir de uma linha ID_STATS.

    A linha traz a contagem na janela e os intervalos mínimo, médio e máximo
    entre quadros do identificador. Três formas de tráfego são distinguidas:

    Episódio isolado: todos os quadros caíram em menos da metade da janela
    (as respostas a um comando do operador, por exemplo). É tratado como uma
    rajada de n quadros a cada janela. Tratar o intervalo entre esses
    quadros como período apontaria uma ocupação que o barramento não tem.

    Rajadas periódicas (transferências segmentadas): intervalos curtos dentro
    da rajada e um intervalo longo entre rajadas. Com b quadros por rajada,
    espaçados de s, e período P:

        dt_avg = P / b        dt_max = P - (b - 1) * s

    o que dá b = (dt_max - s) / (dt_avg - s). O espaçamento s é o menor
    intervalo medido, limitado por baixo pela duração de um quadro: a Probe
    marca o instante em que retira o quadro da fila, e dois quadros seguidos
    podem ser retirados na mesma passagem do laço.

    Um quadro por vez: o período é o menor intervalo medido (modelo
    esporádico, com intervalo mínimo entre chegadas), sem variação de
    liberação. Uma mensagem que falha um ciclo aumenta dt_max, não dt_min, e
    não piora o modelo. Em fluxos densos, cujo intervalo mínimo é da ordem
    da duração de um quadro, o mínimo medido é efeito da fila da Probe e o
    período usado é o intervalo médio.

    O tamanho da rajada é uma estimativa a partir de três números. Vale como
    ordem de grandeza; a tela e a exportação o apresentam como estimado.
    """

    if count <= 0 or window_ms <= 0:
        return None
    window_us = window_ms * 1000.0
    if count < 2 or dt_avg_us <= 0:
        # Uma única ocorrência na janela: mensagem esporádica. O intervalo
        # mínimo entre ocorrências é, no mínimo, o da janela observada.
        return Flow(can_id, name, dlc, period_us=window_us, observed_frames=count, has_deadline=has_deadline)

    frame_us = frame_bits_worst(dlc) * 1_000_000.0 / (bitrate if bitrate > 0 else DEFAULT_BITRATE)
    span_us = (count - 1) * float(dt_avg_us)
    if span_us < window_us / 2.0 and dt_max_us < window_us / 2.0:
        return Flow(can_id, name, dlc, period_us=window_us, burst=min(MAX_BURST, count), observed_frames=count,
                    has_deadline=has_deadline, estimated=True)

    spacing = max(float(dt_min_us), frame_us)
    if spacing * 4.0 < dt_avg_us and dt_max_us > 1.5 * dt_avg_us:
        burst = round((dt_max_us - spacing) / (dt_avg_us - spacing))
        burst = max(2, min(MAX_BURST, count, burst))
        return Flow(can_id, name, dlc, period_us=burst * float(dt_avg_us), burst=burst, observed_frames=count,
                    has_deadline=has_deadline, estimated=True)

    period = float(dt_avg_us) if dt_min_us < 2.0 * frame_us else float(dt_min_us)
    return Flow(can_id, name, dlc, period_us=period, observed_frames=count, has_deadline=has_deadline)


def _fixed_point(start: float, step, limit: float) -> float | None:
    value = start
    for _ in range(MAX_ITERATIONS):
        following = step(value)
        if following > limit:
            return None
        if abs(following - value) < 1e-6:
            return following
        value = following
    return None


def analyze(flows: Iterable[Flow], bitrate: int = DEFAULT_BITRATE) -> Analysis:
    ordered = sorted(flows, key=lambda flow: flow.can_id)
    if bitrate <= 0:
        bitrate = DEFAULT_BITRATE
    tau = 1_000_000.0 / bitrate
    frame = {flow.can_id: frame_bits_worst(flow.dlc) * tau for flow in ordered}
    utilization = sum(flow.burst * frame[flow.can_id] / flow.period_us for flow in ordered if flow.period_us > 0)

    results: list[FlowResult] = []
    for index, flow in enumerate(ordered):
        c_m = frame[flow.can_id]
        higher = ordered[:index]
        lower = ordered[index + 1 :]
        blocking = max((frame[item.can_id] for item in lower), default=0.0)
        # Além deste horizonte o fluxo é dado como não escalonável.
        limit = max(flow.deadline, flow.period_us) * 64.0 + 1_000_000.0

        def demand(window: float, members: Sequence[Flow], extra: float) -> float:
            return sum(
                item.burst * math.ceil((window + item.jitter_us + extra) / item.period_us) * frame[item.can_id]
                for item in members
            )

        level = [*higher, flow]
        busy = _fixed_point(
            blocking + flow.burst * c_m,
            lambda t, blocking=blocking, level=level: blocking + demand(t, level, 0.0),
            limit,
        )
        response_first: float | None = None
        response: float | None = None
        if busy is not None:
            bursts = math.ceil((busy + flow.jitter_us) / flow.period_us)
            if bursts <= MAX_INSTANCES:
                worst = 0.0
                converged = True
                # Dentro de uma rajada o tempo de resposta cresce a cada
                # quadro: basta examinar o primeiro quadro (para informar) e o
                # último de cada rajada do período ocupado.
                candidates = sorted({0, *(k * flow.burst - 1 for k in range(1, bursts + 1))})
                for q in candidates:
                    queued = _fixed_point(
                        blocking + q * c_m,
                        lambda w, q=q, blocking=blocking, c_m=c_m, higher=higher: (
                            blocking + q * c_m + demand(w, higher, tau)
                        ),
                        limit,
                    )
                    if queued is None:
                        converged = False
                        break
                    r_q = flow.jitter_us + queued - (q // flow.burst) * flow.period_us + c_m
                    if q == 0:
                        response_first = r_q
                    worst = max(worst, r_q)
                if converged:
                    response = worst
        results.append(
            FlowResult(
                flow=flow,
                frame_us=c_m,
                utilization=flow.burst * c_m / flow.period_us if flow.period_us > 0 else math.inf,
                blocking_us=blocking,
                response_first_us=response_first if response is not None else None,
                response_us=response,
                deadline_us=flow.deadline,
            )
        )
    return Analysis(tuple(results), bitrate, utilization)


def sensor_template(sensors: int, can_id: int = 0x39F, period_us: float = 1_000_000.0) -> Flow:
    """Tráfego de `sensors` sensores sem fio adicionais, cada um com uma
    amostra de telemetria por período, na faixa de menor prioridade."""

    return Flow(
        can_id,
        f"{sensors} sensor(es) sem fio adicionais",
        8,
        period_us=period_us,
        burst=TELEMETRY_FRAMES * sensors,
        synthetic=True,
    )


@dataclass(frozen=True, slots=True)
class Capacity:
    extra_sensors: int       # quantos cabem mantendo todos os prazos
    limit_reached: bool      # True quando o limite de busca foi atingido
    utilization: float       # ocupação do modelo com esses sensores
    critical: FlowResult | None


def capacity(flows: Iterable[Flow], bitrate: int = DEFAULT_BITRATE, *, maximum: int = 1000,
             period_us: float = 1_000_000.0, utilization_limit: float | None = None) -> Capacity:
    """Quantos sensores sem fio a mais o barramento comporta.

    Procura o maior N para o qual todos os fluxos observados e o tráfego dos
    N sensores novos continuam cumprindo os prazos. Com `utilization_limit`
    (fração de 0 a 1), exige também que a ocupação do modelo fique abaixo
    desse teto: cumprir prazos com o barramento perto de 100 % não deixa
    margem para retransmissões nem para tráfego não previsto.
    """

    base = [replace(flow) for flow in flows if not flow.synthetic]
    used = {flow.can_id for flow in base}
    synthetic_id = max([0x39F, *[can_id + 1 for can_id in used if can_id < 0x7EF]])
    if synthetic_id in used:
        synthetic_id = max(used) + 1

    def fits(count: int) -> Analysis | None:
        analysis = analyze([*base, sensor_template(count, synthetic_id, period_us)], bitrate)
        if utilization_limit is not None and analysis.utilization > utilization_limit:
            return None
        return analysis if analysis.schedulable else None

    if not base:
        return Capacity(0, False, 0.0, None)
    current = analyze(base, bitrate)
    if not current.schedulable or (utilization_limit is not None and current.utilization > utilization_limit):
        return Capacity(0, False, current.utilization, current.critical)

    low, high = 0, maximum
    best: Analysis | None = None
    if (full := fits(maximum)) is not None:
        return Capacity(maximum, True, full.utilization, full.critical)
    while low < high - 1:
        middle = (low + high) // 2
        result = fits(middle) if middle > 0 else current
        if result is not None:
            low, best = middle, result
        else:
            high = middle
    if best is None:
        best = current
    return Capacity(low, False, best.utilization, best.critical)
