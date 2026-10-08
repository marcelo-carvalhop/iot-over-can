"""Telas de métricas da rede e de ensaios de falha.

    Início ─┬─ Métricas da rede (F8 ou b)
            └─ Ensaios (F9 ou e)

As duas telas só leem: as medições vêm de ``pico_tui.metrics.service`` e os
ensaios de ``pico_tui.metrics.experiments``. O envio de comandos fica em
``PicoTuiApp.start_experiment``.
"""
from __future__ import annotations

from collections.abc import Sequence

from rich.text import Text
from textual import on
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.widgets import Label, OptionList, Sparkline, Static

from pico_tui import palette
from pico_tui import presentation as pres
from pico_tui.core.models import AppState
from pico_tui.metrics.experiments import EXPERIMENTS, EXPERIMENTS_BY_KEY, MEASURE_LABELS, ExperimentRun
from pico_tui.metrics.service import IdSample, MetricsSnapshot, SensorLinkMetrics
from pico_tui.screens import BaseScreen, _value, section
from pico_tui.widgets import Fields, Paragraphs, StableOptionList, level_text

LOAD_ATTENTION = 40.0
LOAD_CRITICAL = 70.0


def _decimal(value: float | None, decimals: int = 2, unit: str = "") -> str:
    if value is None:
        return pres.MISSING
    text = f"{value:.{decimals}f}".replace(".", ",")
    return f"{text} {unit}".strip()


def _plural(count: int, singular: str, plural: str) -> str:
    return f"{count} {singular if count == 1 else plural}"


def _milliseconds(microseconds: float | None, decimals: int = 2) -> str:
    return _decimal(None if microseconds is None else microseconds / 1000.0, decimals, "ms")


def _load_text(percent: float) -> Text:
    label = _decimal(percent, 2, "%")
    if percent >= LOAD_CRITICAL:
        return level_text(pres.Level.CRITICAL, label, bold=True)
    if percent >= LOAD_ATTENTION:
        return level_text(pres.Level.ATTENTION, label, bold=True)
    return _value(label, bold=True)


ERROR_STATES = {
    "ERROR_ACTIVE": ("Normal (erro ativo)", pres.Level.NORMAL),
    "ERROR_WARNING": ("Atenção (contador acima de 96)", pres.Level.ATTENTION),
    "ERROR_PASSIVE": ("Erro passivo", pres.Level.CRITICAL),
    "BUS_OFF": ("Fora do barramento (bus-off)", pres.Level.CRITICAL),
}

LINK_STATES = {
    "SECURE": "sessão autenticada",
    "HANDSHAKE": "autenticando",
    "OFFERING": "aguardando o sensor",
    "DOWN": "sem canal de dados",
    "NO_KEY": "módulo sem chave de enlace",
    "NONE": "sem relato do módulo",
}


class MetricsScreen(BaseScreen):
    """Ocupação do barramento, períodos por identificador, tempos de resposta e enlaces."""

    BINDINGS = BaseScreen.BINDINGS + [
        Binding("x", "export", "Exportar", show=False),
        Binding("z", "reset", "Zerar", show=False),
    ]

    def key_hints(self) -> list[tuple[str, str, str | None]]:
        hints: list[tuple[str, str, str | None]] = [
            ("Esc", "Voltar", "screen.back"),
            ("x", "Exportar", "screen.export"),
            ("z", "Zerar medições", "screen.reset"),
            ("F9/e", "Ensaios", "app.show_experiments"),
        ]
        hints.extend(self.tui.global_hints(home=False, commands=False))
        return hints

    def compose_body(self) -> ComposeResult:
        with VerticalScroll(id="metrics-body", classes="body"):
            yield Static(
                "Medições feitas pela Probe 00 no barramento e pelos módulos em cada enlace sem fio. "
                "Os períodos e os tempos de resposta partem do tráfego observado nesta sessão.",
                classes="hint",
            )
            with Horizontal(classes="columns"):
                with Vertical(classes="column column-left"):
                    with section("Barramento", id="sec-met-bus"):
                        yield Fields(id="met-bus")
                        yield Label("Ocupação por segundo", classes="trend-label")
                        yield Sparkline([], id="met-load-trend", classes="trend")
                    with section("Coordenação da rede", id="sec-met-coordination"):
                        yield Fields(id="met-coordination")
                    with section("Comandos aos sensores", id="sec-met-commands"):
                        yield Fields(id="met-commands")
                with Vertical(classes="column"):
                    with section("Tempo de resposta no pior caso", id="sec-met-rta"):
                        yield Fields(id="met-rta")
                    with section("Enlaces sem fio", id="sec-met-links"):
                        yield Paragraphs(id="met-links", empty="Nenhum sensor sem fio com plano de dados.")
                    with section("Acontecimentos", id="sec-met-events"):
                        yield Paragraphs(id="met-events", empty="Nenhuma eleição, troca de líder ou reassociação nesta sessão.")
            with section("Mensagens por identificador", id="sec-met-ids"):
                yield Paragraphs(
                    id="met-ids",
                    empty="Aguardando as estatísticas por identificador (uma leitura a cada 5 s).",
                )

    def breadcrumb(self) -> tuple[Sequence[str], str]:
        return ("Início", "Métricas da rede"), ""

    def update_body(self, state: AppState) -> None:
        self.tui.metrics.set_bitrate(state.gateway.arbitration_bitrate)
        snapshot = self.tui.metrics.snapshot()
        self._update_bus(snapshot)
        self._update_coordination(snapshot)
        self._update_commands(snapshot)
        self._update_analysis(snapshot)
        self._update_links(snapshot)
        self._update_events(snapshot)
        self._update_ids(snapshot)

    def _update_bus(self, snapshot: MetricsSnapshot) -> None:
        bus = snapshot.bus
        fields = self.query_one("#met-bus", Fields)
        if bus is None:
            fields.set_fields(
                [("Medições", "Aguardando a Probe 00. As estatísticas exigem o firmware 0.18 ou posterior.")]
            )
            self.query_one("#met-load-trend", Sparkline).data = []
            return
        state_label, state_level = ERROR_STATES.get(snapshot.error_state, (pres.MISSING, pres.Level.NO_DATA))
        frames_per_second = bus.frames * 1000.0 / bus.window_ms if bus.window_ms else 0.0
        fields.set_fields(
            [
                ("Ocupação (1 s)", _load_text(bus.load_percent)),
                ("Pico (100 ms)", _decimal(bus.peak_percent, 2, "%")),
                (
                    "Máximo na sessão",
                    f"{_decimal(snapshot.max_load_percent, 2, '%')}, pico de {_decimal(snapshot.max_peak_percent, 2, '%')}",
                ),
                ("Quadros por segundo", _decimal(frames_per_second, 1)),
                ("Taxa do barramento", f"{snapshot.bitrate // 1000} kbit/s"),
                ("Identificadores ativos", pres.fmt_int(bus.ids)),
                ("Estado de erro", level_text(state_level, state_label, bold=state_level != pres.Level.NORMAL)),
                (
                    "Contadores de erro",
                    f"recepção {bus.rx_errors}, transmissão {bus.tx_errors} "
                    f"(máximos {snapshot.max_rx_errors} e {snapshot.max_tx_errors})",
                ),
                ("Fila de recepção", f"pico de {bus.rx_queue_peak} quadros"),
                ("Transferências", f"{bus.transfers_ok} concluídas, {bus.transfers_err} descartadas"),
                ("Quadros da própria Probe", f"{bus.own_frames} na última janela"),
            ]
        )
        self.query_one("#met-load-trend", Sparkline).data = [sample.load_percent for sample in snapshot.bus_history[-120:]]

    def _update_coordination(self, snapshot: MetricsSnapshot) -> None:
        coordination = snapshot.coordination
        faults = ", ".join(f"{node:02d}" for node in sorted(coordination.nodes_in_fault)) or "Nenhum"
        last = pres.MISSING
        if coordination.last_heartbeat_t is not None:
            age = max(0.0, snapshot.now - coordination.last_heartbeat_t)
            last = "agora" if age < 1.5 else f"há {_decimal(age, 1, 's')}"
        self.query_one("#met-coordination", Fields).set_fields(
            [
                ("Líder", f"Módulo {coordination.leader:02d}" if coordination.leader else pres.MISSING),
                (
                    "Sinal de presença",
                    f"a cada {coordination.heartbeat_period_ms} ms" if coordination.heartbeat_period_ms else pres.MISSING,
                ),
                ("Último sinal", last),
                ("Sinais recebidos", pres.fmt_int(coordination.heartbeats)),
                ("Maior silêncio do líder", _decimal(coordination.max_heartbeat_gap_s, 2, "s")),
                ("Trocas de líder", pres.fmt_int(coordination.leader_changes)),
                ("Eleições solicitadas", pres.fmt_int(coordination.elections)),
                ("Faltas do líder", pres.fmt_int(coordination.leader_failures)),
                ("Pedidos de entrada", pres.fmt_int(coordination.joins)),
                ("Reassociações decididas", pres.fmt_int(coordination.failovers)),
                ("Módulos em falha", faults),
            ]
        )

    def _update_commands(self, snapshot: MetricsSnapshot) -> None:
        commands = snapshot.commands
        if commands.count == 0:
            fields = [("Tempo de resposta", "Nenhum comando confirmado nesta sessão.")]
        else:
            fields = [
                ("Comandos confirmados", pres.fmt_int(commands.count)),
                ("Tempo mínimo", _decimal(commands.minimum, 0, "ms")),
                ("Tempo médio", _decimal(commands.average, 1, "ms")),
                ("Percentil 95", _decimal(commands.p95, 0, "ms")),
                ("Tempo máximo", _decimal(commands.maximum, 0, "ms")),
            ]
        fields.append(("Comandos recusados", pres.fmt_int(commands.rejected)))
        self.query_one("#met-commands", Fields).set_fields(fields)

    def _update_analysis(self, snapshot: MetricsSnapshot) -> None:
        analysis = snapshot.analysis
        fields = self.query_one("#met-rta", Fields)
        if analysis is None:
            fields.set_fields([("Análise", "Aguardando as estatísticas por identificador.")])
            return
        late = [result for result in analysis.results if not result.schedulable]
        if late:
            verdict = level_text(pres.Level.CRITICAL, f"{len(late)} mensagem(ns) fora do prazo", bold=True)
        else:
            verdict = level_text(pres.Level.NORMAL, "Todos os prazos cumpridos", bold=True)
        rows: list[tuple[str, Text | str]] = [
            ("Método", "Davis et al. (2007), com os períodos medidos"),
            ("Ocupação do modelo", f"{_decimal(100.0 * analysis.utilization, 2, '%')}, com o pior preenchimento de bits"),
            ("Prazos", verdict),
        ]
        critical = analysis.critical
        if critical is not None:
            response = _milliseconds(critical.response_us) if critical.response_us is not None else "não converge"
            rows.append(
                (
                    "Menor folga",
                    f"{critical.flow.name}: resposta de {response} para um prazo de "
                    f"{_milliseconds(critical.deadline_us, 0)}",
                )
            )
        capacity = snapshot.capacity
        if capacity is not None:
            more = "mais de " if capacity.limit_reached else ""
            rows.append(
                (
                    "Capacidade restante",
                    f"{more}{capacity.extra_sensors} sensores sem fio a uma amostra por segundo, "
                    f"com o barramento em {_decimal(100.0 * capacity.utilization, 0, '%')}",
                )
            )
        guideline = snapshot.capacity_guideline
        if guideline is not None:
            rows.append(
                (
                    "Com teto de 40 %",
                    f"{guideline.extra_sensors} sensores sem fio, mantendo a ocupação do barramento abaixo de 40 %",
                )
            )
        rows.append(("Ressalva", "Vale para o tráfego observado; ver Documentação, métricas e ensaios."))
        fields.set_fields(rows)

    @staticmethod
    def _link_line(link: SensorLinkMetrics) -> Text:
        line = Text(f"Sensor {link.logical_id}", style=f"bold {palette.TEXT_PRIMARY}")
        line.append(f"  {LINK_STATES.get(link.link_state, link.link_state.lower())}", style=palette.TEXT_SECONDARY)
        parts: list[str] = []
        if link.rssi_dbm is not None:
            parts.append(f"sinal Wi-Fi de {link.rssi_dbm} dBm")
        if link.rtt_ms is not None:
            parts.append(
                f"ida e volta de {_decimal(link.rtt_ms, 1, 'ms')} (máximo {_decimal(link.rtt_max_ms, 1, 'ms')})"
            )
        if link.interval_avg_s is not None:
            parts.append(
                f"telemetria a cada {_decimal(link.interval_avg_s, 2, 's')} "
                f"(de {_decimal(link.interval_min_s, 2)} a {_decimal(link.interval_max_s, 2, 's')})"
            )
        parts.append(f"{_plural(link.telemetry_count, 'amostra', 'amostras')}, {link.telemetry_lost} perdidas")
        if link.sessions:
            parts.append(_plural(link.sessions, "sessão", "sessões"))
        if link.last_recovery_s is not None:
            parts.append(f"última retomada em {_decimal(link.last_recovery_s, 1, 's')}")
        if link.auth_failures or link.replay_drops:
            parts.append(f"{link.auth_failures} falhas de autenticação, {link.replay_drops} repetições recusadas")
        line.append("  " + "; ".join(parts), style=palette.TEXT_PRIMARY)
        return line

    def _update_links(self, snapshot: MetricsSnapshot) -> None:
        self.query_one("#met-links", Paragraphs).set_items([self._link_line(link) for link in snapshot.links], indent=2)

    def _update_events(self, snapshot: MetricsSnapshot) -> None:
        items: list[Text] = []
        for event in snapshot.timeline[::-1][:10]:
            line = Text(pres.fmt_clock(event.wall), style=palette.TEXT_DIM)
            line.append(f"  {event.text}", style=palette.TEXT_PRIMARY)
            items.append(line)
        self.query_one("#met-events", Paragraphs).set_items(items, indent=2)

    def _update_ids(self, snapshot: MetricsSnapshot) -> None:
        results = {result.flow.can_id: result for result in (snapshot.analysis.results if snapshot.analysis else ())}
        items: list[Text] = []
        for sample in snapshot.ids:
            items.append(self._id_line(sample, results.get(sample.can_id)))
        self.query_one("#met-ids", Paragraphs).set_items(items, indent=7)

    @staticmethod
    def _id_line(sample: IdSample, result) -> Text:
        line = Text(f"0x{sample.can_id:03X}", style=f"bold {palette.TEXT_PRIMARY}")
        line.append(f"  {sample.name}", style=palette.TEXT_SECONDARY)
        parts: list[str] = [
            f"{_plural(sample.count, 'quadro', 'quadros')} em {_decimal(sample.window_ms / 1000.0, 1, 's')}"
        ]
        if result is not None:
            flow = result.flow
            if flow.burst > 1:
                parts.append(
                    f"rajadas de cerca de {flow.burst} quadros a cada {_milliseconds(flow.period_us, 0)} (estimado)"
                )
            elif sample.count >= 2:
                parts.append(
                    f"intervalo mínimo de {_milliseconds(flow.period_us, 1)}, "
                    f"variação de {_milliseconds(sample.jitter_us, 2)}"
                )
            else:
                parts.append("esporádica")
            if not flow.has_deadline:
                parts.append("sem prazo")
            elif result.response_us is None:
                parts.append("pior resposta não converge")
            else:
                parts.append(f"pior resposta de {_milliseconds(result.response_us)}")
        line.append("  " + "; ".join(parts), style=palette.TEXT_PRIMARY)
        if result is not None and not result.schedulable:
            line.append("  fora do prazo", style=f"bold {palette.STATE_CRITICAL}")
        return line

    def action_export(self) -> None:
        self.tui.export_metrics()

    def action_reset(self) -> None:
        self.tui.reset_metrics()


class ExperimentsScreen(BaseScreen):
    """Ensaios de injeção de falhas, com medição automática do resultado."""

    BINDINGS = BaseScreen.BINDINGS + [
        Binding("s", "stop", "Encerrar", show=False),
        Binding("x", "export", "Exportar", show=False),
    ]

    def key_hints(self) -> list[tuple[str, str, str | None]]:
        hints: list[tuple[str, str, str | None]] = [
            ("Esc", "Voltar", "screen.back"),
            ("Enter", "Iniciar ensaio", None),
            ("s", "Encerrar ensaio", "screen.stop"),
            ("x", "Exportar", "screen.export"),
            ("F8/b", "Métricas", "app.show_metrics"),
        ]
        hints.extend(self.tui.global_hints(home=False, commands=False))
        return hints

    def compose_body(self) -> ComposeResult:
        with VerticalScroll(id="experiments-body", classes="body"):
            yield Static(
                "Cada ensaio provoca uma falha em um módulo ou no barramento e mede a reação da rede. "
                "Os comandos exigem a mesma autorização dos demais comandos de manutenção.",
                classes="hint",
            )
            with Horizontal(classes="columns"):
                with Vertical(classes="column column-left column-main"):
                    with section("Ensaios disponíveis", id="sec-exp-list"):
                        yield StableOptionList(id="exp-list", empty="Nenhum ensaio disponível.")
                with Vertical(classes="column column-side"):
                    with section("O que este ensaio faz", id="sec-exp-details"):
                        yield Paragraphs(id="exp-details", empty="Realce um ensaio para ver a descrição.")
            with section("Ensaio em curso", id="sec-exp-active"):
                yield Fields(id="exp-active")
                yield Paragraphs(id="exp-timeline", empty="")
            with section("Resultados", id="sec-exp-results"):
                yield Paragraphs(id="exp-results", empty="Nenhum ensaio concluído nesta sessão.")

    def breadcrumb(self) -> tuple[Sequence[str], str]:
        return ("Início", "Ensaios"), "injeção de falhas"

    def on_mount(self) -> None:
        super().on_mount()
        widget = self.query_one("#exp-list", StableOptionList)
        items: list[tuple[str, Text]] = []
        for experiment in EXPERIMENTS:
            lines = [
                Text(f"  {experiment.title}", style=f"bold {palette.TEXT_PRIMARY}"),
                Text(f"  {self._target_hint(experiment.target, experiment.kind)}", style=palette.TEXT_MUTED),
            ]
            items.append((experiment.key, Text("\n").join(lines)))
        widget.set_items(items)
        widget.focus()
        self._show_details(widget.highlighted_id)

    @staticmethod
    def _target_hint(target: str, kind: str) -> str:
        if kind == "MANUAL":
            return "sem comando"
        return {
            "leader": "alvo: o líder",
            "owner": "alvo: um módulo",
            "node": "alvo: um módulo",
            "none": "alvo: o barramento",
        }.get(target, "")

    def _show_details(self, key: str | None) -> None:
        details = self.query_one("#exp-details", Paragraphs)
        experiment = EXPERIMENTS_BY_KEY.get(key or "")
        if experiment is None:
            details.set_items([])
            return
        lines = [Text(experiment.summary, style=palette.TEXT_PRIMARY)]
        if experiment.effect:
            lines.append(Text(f"Efeito: {experiment.effect}", style=palette.TEXT_SECONDARY))
        measured = "; ".join(MEASURE_LABELS[name][0].lower() for name in experiment.measures)
        lines.append(Text(f"Mede: {measured}.", style=palette.TEXT_SECONDARY))
        if experiment.after:
            lines.append(Text(f"Depois: {experiment.after}", style=palette.ACCENT_FOCUS))
        details.set_items(lines, indent=0)

    @on(OptionList.OptionHighlighted, "#exp-list")
    def _highlighted(self, event: OptionList.OptionHighlighted) -> None:
        self._show_details(str(event.option.id) if event.option.id else None)

    @on(OptionList.OptionSelected, "#exp-list")
    def _selected(self, event: OptionList.OptionSelected) -> None:
        key = str(event.option.id or "")
        if key in EXPERIMENTS_BY_KEY:
            self.tui.start_experiment(key)

    def update_body(self, state: AppState) -> None:
        # O encerramento por prazo é feito pelo aplicativo a cada ciclo, com a
        # tela aberta ou não (PicoTuiApp._poll_experiment).
        recorder = self.tui.experiments
        run = recorder.active
        active = self.query_one("#exp-active", Fields)
        timeline = self.query_one("#exp-timeline", Paragraphs)
        if run is None:
            active.set_fields([("Situação", "Nenhum ensaio em curso. Escolha um na lista e pressione Enter.")])
            timeline.set_items([])
        else:
            elapsed = recorder.elapsed()
            if run.experiment.kind == "MANUAL":
                progress = f"{_decimal(elapsed, 0, 's')} decorridos; pressione s para encerrar"
            else:
                remaining = max(0.0, run.planned_seconds - elapsed)
                progress = (
                    f"{_decimal(elapsed, 0, 's')} decorridos, mais {_decimal(remaining, 0, 's')} de observação"
                )
            rows: list[tuple[str, Text | str]] = [
                ("Ensaio", _value(run.title, bold=True)),
                ("Andamento", progress),
            ]
            if run.origin == "OBSERVED":
                rows.append(("Origem", "iniciado fora desta tela e reconhecido pelas mensagens da Probe 00"))
            active.set_fields(rows)
            lines: list[Text] = []
            for label, value in run.rows():
                if value != "não observado":
                    line = Text(f"{label}: ", style=palette.TEXT_MUTED)
                    line.append(value, style=f"bold {palette.TEXT_PRIMARY}")
                    lines.append(line)
            lines.extend(self._event_line(event.t, event.text) for event in run.events[-8:])
            timeline.set_items(lines, indent=4)
        self.query_one("#exp-results", Paragraphs).set_items(self._result_items(list(recorder.history)), indent=4)

    @staticmethod
    def _event_line(seconds: float, text: str) -> Text:
        line = Text(_decimal(seconds, 2, "s"), style=palette.TEXT_DIM)
        line.append(f"  {text}", style=palette.TEXT_PRIMARY)
        return line

    @staticmethod
    def _result_items(runs: list[ExperimentRun]) -> list[Text]:
        items: list[Text] = []
        for run in runs[:6]:
            header = Text(pres.fmt_clock(run.started_wall), style=palette.TEXT_DIM)
            header.append(f"  {run.title}", style=f"bold {palette.TEXT_PRIMARY}")
            if run.state == "STOPPED":
                header.append("  encerrado pelo operador", style=palette.TEXT_MUTED)
            items.append(header)
            for label, value in run.rows():
                line = Text(f"    {label}: ", style=palette.TEXT_MUTED)
                line.append(value, style=palette.TEXT_PRIMARY if value != "não observado" else palette.TEXT_MUTED)
                items.append(line)
        return items

    def action_stop(self) -> None:
        self.tui.stop_experiment()

    def action_export(self) -> None:
        self.tui.export_metrics()
