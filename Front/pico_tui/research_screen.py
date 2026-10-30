"""Tela de pesquisa ("/"): medições da instrumentação para o TCC.

Não faz parte da operação da rede e por isso não aparece na barra de teclas;
abre com "/" e está listada na Ajuda. Só lê: os números vêm de
``pico_tui.research.service``. Os comandos que geram as medições (ECHO,
FAULT LOAD, CONFIG, BENCH CRYPTO) são enviados pelo lançador de comandos
("k"), e as campanhas com repetições continuam em ``Pesquisa/``.
"""
from __future__ import annotations

import math
from collections.abc import Sequence

from rich.text import Text
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.widgets import Label, Sparkline, Static

from pico_tui import palette
from pico_tui import presentation as pres
from pico_tui.core.models import AppState
from pico_tui.research.service import EchoTrain, LinkStats, ResearchSnapshot
from pico_tui.research.stats import Summary, summarize
from pico_tui.screens import BaseScreen, section
from pico_tui.widgets import Fields, Paragraphs

PATH_LABELS = {
    "LOCAL": "Eco LOCAL: só o caminho CAN do plano de dados",
    "SENSOR": "Eco SENSOR: até o sensor e de volta",
}

EXAMPLES = (
    ("CONFIG", "parâmetros da compilação da Probe (L, H, T, IDs de carga)"),
    ("ECHO 01.01 40 500 LOCAL 7", "40 ecos pelo CAN até o Módulo 01, intervalos de Poisson com média de 500 ms, semente 7"),
    ("ECHO 01.01 40 500 SENSOR 8", "o mesmo, passando pelo sensor 01.01 (exige sessão)"),
    ("ECHO CANCEL", "interrompe o trem em andamento"),
    ("FAULT LOAD 50 60 MID 42", "carga de fundo de 50 % por 60 s, prioridade intermediária, semente 42"),
    ("BENCH CRYPTO 200", "custo da criptografia na Probe (pausa a Probe por alguns décimos de segundo)"),
)


def _num(value: float | None, decimals: int = 2) -> str:
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return pres.MISSING
    return f"{value:.{decimals}f}".replace(".", ",")


def _ms(value_us: float | None, decimals: int = 2) -> str:
    if value_us is None or math.isnan(value_us):
        return pres.MISSING
    return f"{_num(value_us / 1000.0, decimals)} ms"


def _us(value: float | None, decimals: int = 0) -> str:
    if value is None or math.isnan(value):
        return pres.MISSING
    return f"{_num(value, decimals)} µs"


def _distribution(summary: Summary, unit=_ms) -> list[tuple[str, str]]:
    """Média com IC 95 %, quantis e extremos de uma série."""

    if not summary.valid:
        return [("Amostras", "nenhuma")]
    rows = [("Amostras", pres.fmt_int(summary.n))]
    if summary.n >= 2:
        rows.append(("Média (IC 95 %)", f"{unit(summary.mean)} ({unit(summary.ci_low)} a {unit(summary.ci_high)})"))
        rows.append(("Desvio padrão", unit(summary.stdev)))
    else:
        rows.append(("Média", unit(summary.mean)))
    rows.append(("Mediana · p95 · p99", f"{unit(summary.p50)} · {unit(summary.p95)} · {unit(summary.p99)}"))
    rows.append(("Mínimo · máximo", f"{unit(summary.minimum)} · {unit(summary.maximum)}"))
    return rows


class ResearchScreen(BaseScreen):
    """Eco, sinal de presença, ocupação, custo da segurança e parâmetros da compilação."""

    BINDINGS = BaseScreen.BINDINGS + [
        Binding("z", "reset", "Zerar", show=False),
    ]

    def key_hints(self) -> list[tuple[str, str, str | None]]:
        hints: list[tuple[str, str, str | None]] = [
            ("Esc", "Voltar", "screen.back"),
            ("k", "Enviar comando", "app.command_launcher"),
            ("z", "Zerar medições", "screen.reset"),
        ]
        hints.extend(self.tui.global_hints(home=False, commands=False))
        return hints

    def compose_body(self) -> ComposeResult:
        with VerticalScroll(id="research-body", classes="body"):
            yield Static(
                "Medições para pesquisa (TCC), fora da operação normal da rede. Os números são desta sessão; "
                "campanhas com repetições, comparações e proveniência são feitas com Pesquisa/ (python -m iocpesquisa).",
                classes="hint",
            )
            with Horizontal(classes="columns"):
                with Vertical(classes="column column-left"):
                    with section("Eco LOCAL: só o caminho CAN", id="sec-rs-local"):
                        yield Fields(id="rs-local")
                        yield Label("Ida e volta de cada eco", classes="trend-label")
                        yield Sparkline([], id="rs-local-trend", classes="trend")
                    with section("Eco SENSOR: até o sensor e de volta", id="sec-rs-sensor"):
                        yield Fields(id="rs-sensor")
                        yield Label("Ida e volta de cada eco", classes="trend-label")
                        yield Sparkline([], id="rs-sensor-trend", classes="trend")
                    with section("Sinal de presença do líder (relógio da Probe)", id="sec-rs-heartbeat"):
                        yield Fields(id="rs-heartbeat")
                    with section("Ocupação do barramento e carga de fundo", id="sec-rs-bus"):
                        yield Fields(id="rs-bus")
                with Vertical(classes="column"):
                    with section("Custo da segurança e da ponte, medido nos Nodes", id="sec-rs-links"):
                        yield Paragraphs(
                            id="rs-links",
                            empty="Nenhum relatório WIRELESS_STATS ainda (um a cada 5 s por sensor com sessão).",
                        )
                    with section("Custo da criptografia na Probe (BENCH CRYPTO)", id="sec-rs-bench"):
                        yield Paragraphs(id="rs-bench", empty="Envie BENCH CRYPTO pelo lançador (k) para medir.")
                    with section("Parâmetros da compilação (CONFIG)", id="sec-rs-config"):
                        yield Fields(id="rs-config")
                    with section("Trens de eco desta sessão", id="sec-rs-trains"):
                        yield Paragraphs(id="rs-trains", empty="Nenhum trem de eco nesta sessão.")
            with section("Comandos de pesquisa (k abre o lançador)", id="sec-rs-commands"):
                yield Paragraphs(id="rs-commands")

    def breadcrumb(self) -> tuple[Sequence[str], str]:
        return ("Início", "Pesquisa"), "medições para o TCC"

    def on_mount(self) -> None:
        super().on_mount()
        items = []
        for command, meaning in EXAMPLES:
            line = Text()
            line.append(command, style=f"bold {palette.ACCENT_FOCUS}")
            line.append(f"  {meaning}", style=palette.TEXT_SECONDARY)
            items.append(line)
        self.query_one("#rs-commands", Paragraphs).set_items(items, indent=4)

    def update_body(self, state: AppState) -> None:
        snapshot = self.tui.research.snapshot()
        self._update_echo("LOCAL", snapshot)
        self._update_echo("SENSOR", snapshot)
        self._update_heartbeat(snapshot)
        self._update_bus(snapshot)
        self._update_links(snapshot)
        self._update_bench(snapshot)
        self._update_config(snapshot)
        self._update_trains(snapshot)

    # ------------------------------------------------------------ eco

    def _update_echo(self, path: str, snapshot: ResearchSnapshot) -> None:
        key = path.lower()
        fields = self.query_one(f"#rs-{key}", Fields)
        trend = self.query_one(f"#rs-{key}-trend", Sparkline)
        train = snapshot.latest_train(path)
        if train is None:
            hint = f"Nenhum trem ainda. Exemplo: ECHO 01.01 40 500 {path} 7"
            if snapshot.echo_error:
                hint = f"A Probe recusou: {snapshot.echo_error}"
            fields.set_fields([("Medição", hint)])
            trend.data = []
            return
        rows: list[tuple[str, Text | str]] = [("Alvo", self._train_label(train))]
        progress = f"{train.sent} de {train.count} enviados"
        if train.active:
            progress += ", em andamento"
        elif train.finished_wall is not None:
            progress += f", concluído às {pres.fmt_clock(train.finished_wall)}"
        rows.append(("Andamento", progress))
        if train.sent:
            proportion, low, high = train.loss()
            rows.append(
                (
                    "Sem resposta",
                    f"{train.lost} de {train.sent} ({_num(100 * proportion, 1)} %, IC 95 % "
                    f"{_num(100 * low, 1)} a {_num(100 * high, 1)} %)",
                )
            )
        rows.extend(_distribution(train.rtt()))
        if path == "SENSOR":
            wifi, can = train.wifi(), train.can_part()
            if wifi.valid:
                rows.append(("Parte Wi-Fi (Node)", f"mediana {_ms(wifi.p50)}, p99 {_ms(wifi.p99)}"))
                rows.append(("CAN e Node", f"mediana {_ms(can.p50)}, p99 {_ms(can.p99)}"))
        fields.set_fields(rows)
        trend.data = train.rtt_series()[-120:]

    @staticmethod
    def _train_label(train: EchoTrain) -> str:
        sampling = "intervalos de Poisson" if train.sampling == "POISSON" else "período fixo"
        seed = f", semente {train.seed}" if train.seed else ""
        return f"Sensor {train.target}, {sampling} com média de {train.period_ms} ms{seed}"

    def _update_trains(self, snapshot: ResearchSnapshot) -> None:
        items: list[Text] = []
        for train in reversed(snapshot.trains):
            summary = train.rtt()
            line = Text()
            line.append(pres.fmt_clock(train.started_wall), style=palette.TEXT_DIM)
            line.append(f"  {train.path}", style=f"bold {palette.TEXT_PRIMARY}")
            line.append(f"  {train.target}", style=palette.TEXT_SECONDARY)
            parts = [f"{train.received}/{train.sent} respostas"]
            if summary.valid:
                parts.append(f"mediana {_ms(summary.p50)}, p99 {_ms(summary.p99)}, máximo {_ms(summary.maximum)}")
            if train.active:
                parts.append("em andamento")
            line.append("  " + "; ".join(parts), style=palette.TEXT_PRIMARY)
            items.append(line)
        self.query_one("#rs-trains", Paragraphs).set_items(items, indent=4)

    # ------------------------------------------------------------ rede

    def _update_heartbeat(self, snapshot: ResearchSnapshot) -> None:
        fields = self.query_one("#rs-heartbeat", Fields)
        intervals = snapshot.heartbeat_intervals_us
        if not intervals:
            reason = (
                "A Probe não informa o relógio (t_us): firmware anterior à instrumentação de pesquisa."
                if snapshot.heartbeats_without_clock
                else "Aguardando o sinal de presença do líder."
            )
            fields.set_fields([("Medição", reason)])
            return
        summary = summarize(intervals)
        rows: list[tuple[str, Text | str]] = [
            ("Período declarado", f"{snapshot.heartbeat_period_ms} ms" if snapshot.heartbeat_period_ms else pres.MISSING),
            ("Intervalos medidos", pres.fmt_int(summary.n)),
            ("Intervalo médio", _ms(summary.mean, 3)),
            ("Variação (desvio padrão)", _us(summary.stdev, 1)),
        ]
        if snapshot.heartbeat_period_ms:
            nominal = snapshot.heartbeat_period_ms * 1000.0
            deviations = summarize([abs(value - nominal) for value in intervals])
            rows.append(("Desvio do período: p99 · máximo", f"{_us(deviations.p99, 1)} · {_us(deviations.maximum, 1)}"))
        rows.append(("Menor · maior intervalo", f"{_ms(summary.minimum, 3)} · {_ms(summary.maximum, 3)}"))
        fields.set_fields(rows)

    def _update_bus(self, snapshot: ResearchSnapshot) -> None:
        rows: list[tuple[str, Text | str]] = []
        if snapshot.bus_loads:
            loads = summarize(snapshot.bus_loads)
            rows.append(("Janelas de 1 s", pres.fmt_int(loads.n)))
            rows.append(("Ocupação média (IC 95 %)", f"{_num(loads.mean)} % ({_num(loads.ci_low)} a {_num(loads.ci_high)} %)"))
            rows.append(("Mediana · p95 · máximo", f"{_num(loads.p50)} · {_num(loads.p95)} · {_num(loads.maximum)} %"))
        else:
            rows.append(("Ocupação", "Aguardando BUS_STATS da Probe."))
        load = snapshot.load
        if load is None:
            rows.append(("Carga de fundo", "nenhuma nesta sessão"))
        else:
            data = load.fields
            state = {"STARTED": "em andamento", "ENDED": "encerrada", "REJECTED": "recusada"}.get(load.state, load.state.lower())
            target = data.get("LOAD_PCT")
            rows.append(
                (
                    "Carga de fundo",
                    f"{target} %, prioridade {str(data.get('PRIORITY', '?')).upper()}, {state} às {pres.fmt_clock(load.wall)}",
                )
            )
            if data.get("ACHIEVED_PCT") is not None:
                rows.append(("Ocupação gerada", f"{str(data.get('ACHIEVED_PCT')).replace('.', ',')} %, {data.get('FRAMES', '?')} quadros"))
            if data.get("SEED") is not None:
                rows.append(("Semente · hash da sequência", f"{data.get('SEED')} · {data.get('SEQ_HASH', pres.MISSING)}"))
        self.query_one("#rs-bus", Fields).set_fields(rows)

    # ------------------------------------------------------------ Nodes

    def _update_links(self, snapshot: ResearchSnapshot) -> None:
        items = [self._link_line(link) for link in snapshot.links]
        self.query_one("#rs-links", Paragraphs).set_items(items, indent=4)

    @staticmethod
    def _link_line(link: LinkStats) -> Text:
        line = Text()
        line.append(f"Sensor {link.logical_id}", style=f"bold {palette.TEXT_PRIMARY}")
        parts: list[str] = [f"{len(link.windows)} janelas de 5 s"]
        rx, tx = link.summary("crypto_rx_us"), link.summary("crypto_tx_us")
        if rx.valid:
            parts.append(f"HMAC na recepção {_us(rx.mean, 0)} em média (p95 {_us(rx.p95, 0)})")
        if tx.valid:
            parts.append(f"selo no envio {_us(tx.mean, 0)}")
        worst = link.maximum("crypto_max_us")
        if worst is not None:
            parts.append(f"pior datagrama {_us(worst, 0)}")
        handshake = link.last_handshake_us()
        if handshake is not None:
            parts.append(f"handshake {_ms(handshake, 2)} de CPU")
        bridge = link.summary("bridge_avg_us")
        if bridge.valid:
            parts.append(
                f"ponte Wi-Fi → CAN {_ms(bridge.mean, 2)} em média, máximo {_ms(link.maximum('bridge_max_us'), 2)}"
            )
        line.append("  " + "; ".join(parts), style=palette.TEXT_PRIMARY)
        return line

    def _update_bench(self, snapshot: ResearchSnapshot) -> None:
        items: list[Text] = []
        for result in snapshot.bench:
            data = result.fields
            line = Text()
            line.append(result.op, style=f"bold {palette.TEXT_PRIMARY}")
            line.append(f"  {data.get('BYTES', '?')} bytes, n={data.get('N', '?')}", style=palette.TEXT_SECONDARY)
            line.append(
                f"  média {data.get('MEAN_US', '?').replace('.', ',')} µs; mediana {data.get('P50_US', '?')}, "
                f"p95 {data.get('P95_US', '?')}, p99 {data.get('P99_US', '?')}, máximo {data.get('MAX_US', '?')} µs",
                style=palette.TEXT_PRIMARY,
            )
            items.append(line)
        if items and snapshot.bench_cpu_mhz:
            items.append(Text(f"CPU a {snapshot.bench_cpu_mhz} MHz", style=palette.TEXT_MUTED))
        self.query_one("#rs-bench", Paragraphs).set_items(items, indent=4)

    def _update_config(self, snapshot: ResearchSnapshot) -> None:
        fields = self.query_one("#rs-config", Fields)
        config = snapshot.config
        if config is None:
            fields.set_fields([("Parâmetros", "Envie CONFIG pelo lançador (k); a Probe também os publica ao ligar.")])
            return

        def get(key: str, unit: str = "") -> str:
            value = config.get(key)
            return f"{value} {unit}".strip() if value is not None else pres.MISSING

        fields.set_fields(
            [
                ("Taxa do barramento", f"{int(config['BITRATE']) // 1000} kbit/s" if config.get("BITRATE", "").isdigit() else get("BITRATE")),
                ("Sinal de presença", get("HB_PERIOD_MS", "ms")),
                ("L: vínculo em STALE após", get("ASSOC_STALE_MS", "ms")),
                ("De STALE a LOST", get("ASSOC_LOST_MS", "ms")),
                ("H: histerese da volta", get("ASSOC_HYSTERESIS_MS", "ms")),
                ("T: período da telemetria", get("STREAM_PERIOD_MS", "ms")),
                ("PING e fim de sessão", f"{get('PING_MS', 'ms')} e {get('SESSION_TIMEOUT_MS', 'ms')}"),
                ("IDs de carga", get("LOAD_IDS")),
                ("Injeção de falhas", "habilitada" if config.get("FAULT_INJECTION") == "1" else "desabilitada"),
                ("Lido às", pres.fmt_clock(snapshot.config_wall) if snapshot.config_wall else pres.MISSING),
            ]
        )

    def action_reset(self) -> None:
        self.tui.research.reset()
        self.tui.refresh_now()
        self.tui.notify("Medições de pesquisa zeradas.", timeout=3)
