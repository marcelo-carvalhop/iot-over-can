"""Supervisório: até cinco painéis na mesma tela, escolhidos pelo operador.

    Início ── Supervisório (F12 ou v)

Cada sensor entra no máximo uma vez; o painel "Rede e barramento" resume as
métricas da Probe 00 (ocupação, erros, líder e enlaces sem fio) e o painel
"Módulos CAN" a condição de cada módulo. A borda de cada painel toma a cor da
condição do que ele mostra (âmbar em atenção, vermelho em crítico). A composição
fica nas preferências (``supervisor_panels`` em tui.json) e volta na próxima
sessão; um sensor ausente continua no painel, à espera de dados.
"""
from __future__ import annotations

import re
from collections.abc import Sequence

from rich.text import Text
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Container, VerticalScroll
from textual.css.query import NoMatches
from textual.widgets import Label, Sparkline, Static

from pico_tui import acoustic_view, palette
from pico_tui import presentation as pres
from pico_tui.core.models import AppState, SensorNode
from pico_tui.dialogs import ChoiceScreen
from pico_tui.metrics.catalog import WIRELESS_DATA_BASE
from pico_tui.metrics.service import MetricsSnapshot
from pico_tui.metrics_screens import (
    ERROR_STATES,
    LINK_STATES,
    LOAD_ATTENTION,
    LOAD_CRITICAL,
    MetricsScreen,
    _decimal,
    _load_text,
)
from pico_tui.screens import BaseScreen, NetworkScreen, _condition_value, _value
from pico_tui.widgets import Fields, KeyBar, level_text

PANEL_LIMIT = 5
NETWORK_PANEL = "rede"
NODES_PANEL = "modulos"
SENSOR_KEY = re.compile(r"^sensor:\d{2}\.\d{2}$")
LEVEL_CLASSES = {pres.Level.ATTENTION: "-attention", pres.Level.CRITICAL: "-critical"}

Field = tuple[str, Text | str]

# Do sensor acústico, o painel mostra os níveis principais e o essencial do modo.
ACOUSTIC_LABELS = frozenset(
    {
        "LAeq (último segundo)",
        "LCeq / LZeq",
        "Pico (sem ponderação)",
        "Dose NR-15 (q = 5)",
        "Dose NHO-01 (q = 3)",
        "Critério da NHO-01",
        "Fundamental (f0)",
        "Rotação estimada",
        "Escore de anomalia (z)",
        "Eventos desde o reinício",
        "Razão STA/LTA",
        "Último evento",
    }
)


def valid_key(key: object) -> bool:
    return key in (NETWORK_PANEL, NODES_PANEL) or (isinstance(key, str) and bool(SENSOR_KEY.match(key)))


def find_sensor(state: AppState, ident: str) -> SensorNode | None:
    parent, _, child = ident.partition(".")
    try:
        node = state.nodes.get(int(parent))
        return node.sensors.get(int(child)) if node else None
    except ValueError:
        return None


def panel_options(state: AppState, current: Sequence[str]) -> list[tuple[str, str]]:
    """Painéis que ainda podem entrar: a rede e cada sensor, uma vez só."""

    options: list[tuple[str, str]] = []
    if NETWORK_PANEL not in current:
        options.append(("Rede e barramento: ocupação, erros, líder e enlaces sem fio", NETWORK_PANEL))
    if NODES_PANEL not in current:
        options.append(("Módulos CAN: condição, papel e sensores de cada módulo", NODES_PANEL))
    for node_id in sorted(state.nodes):
        for _child, sensor in sorted(state.nodes[node_id].sensors.items()):
            key = f"sensor:{sensor.logical_id}"
            if key not in current:
                options.append(
                    (f"Sensor {sensor.logical_id}: {pres.profile_label(sensor.profile_id)}, Módulo {node_id:02d}", key)
                )
    return options


def grid_shape(count: int, *, narrow: bool, wide: bool) -> tuple[int, int]:
    """(colunas, linhas) da grade; o último painel ocupa as células que sobrarem."""

    if count <= 1:
        return 1, 1
    if narrow:
        return 1, count
    columns = {2: 2, 3: 3, 4: 2}.get(count, 3) if wide else 2
    return columns, -(-count // columns)


# ---------------------------------------------------------------------------
# Conteúdo dos painéis
# ---------------------------------------------------------------------------


def sensor_fields(sensor: SensorNode) -> list[Field]:
    fields: list[Field] = [
        ("Condição", _condition_value(pres.sensor_condition(sensor))),
        ("Comunicação", f"{pres.status_label(sensor.status)}, última leitura {pres.fmt_age(pres.sensor_age(sensor))}"),
        ("Modo", pres.sensor_mode_label(sensor.sensor_mode.value)),
    ]
    if (sensor.profile_id or "").upper() == "ACOUSTIC":
        metrics = acoustic_view.latest(sensor)
        if metrics is not None:
            rows = acoustic_view.level_fields(metrics) + acoustic_view.mode_fields(sensor, metrics)
            fields += [row for row in rows if row[0] in ACOUSTIC_LABELS]
    elif (sample := sensor.latest_telemetry) is not None:
        trigger = pres.MISSING if sample.stalta_triggered is None else ("Disparado" if sample.stalta_triggered else "Em repouso")
        fields += [
            ("RMS", _value(pres.fmt_number(sample.rms, 4, sample.rms_unit), bold=True)),
            ("PPV", _value(pres.fmt_number(sample.ppv_mm_s, 3, "mm/s"), bold=True)),
            (
                "Frequência dominante",
                "Não calculada neste modo" if sample.fft_valid is False else pres.fmt_number(sample.peak_frequency_hz, 2, "Hz"),
            ),
            ("Gatilho STA/LTA", trigger),
        ]
        if sample.clipping:
            fields.append(("Saturação", level_text(pres.Level.ATTENTION, "Sim, sinal saturado", bold=True)))
    codes = sorted(code for code, record in sensor.active_dtcs.items() if record.active)
    if codes:
        fields.append(("DTC ativos", ", ".join(f"0x{code:04X}" for code in codes)))
    if sensor.wireless_associated:
        link = [pres.data_link_label(sensor.data_link_state).capitalize()]
        if sensor.data_link_rssi_dbm is not None:
            link.append(f"{sensor.data_link_rssi_dbm} dBm")
        if sensor.data_link_rtt_ms is not None:
            link.append(f"ida e volta {pres.fmt_number(sensor.data_link_rtt_ms, 1, 'ms')}")
        fields.append(("Enlace sem fio", ", ".join(link)))
    if sensor.rx_count + sensor.lost_count:
        fields.append(
            (
                "Leituras / perdidas",
                (
                    f"{pres.fmt_int(sensor.rx_count)} / {pres.fmt_int(sensor.lost_count)} "
                    f"({pres.fmt_number(sensor.loss_percent, 2, '%')})"
                ),
            )
        )
    return fields


def sensor_trend(sensor: SensorNode) -> tuple[str, list[float]]:
    if (sensor.profile_id or "").upper() == "ACOUSTIC":
        return "Tendência do LAeq", acoustic_view.laeq_trend(sensor)
    history = list(sensor.telemetry_history)[-120:]
    return "Tendência do RMS", [item.rms for item in history if item.rms is not None]


def network_fields(snapshot: MetricsSnapshot) -> list[Field]:
    bus = snapshot.bus
    fields: list[Field] = []
    if bus is None:
        fields.append(("Barramento", "Aguardando as medições da Probe 00 (firmware 0.18 ou posterior)"))
    else:
        state_label, state_level = ERROR_STATES.get(snapshot.error_state, (pres.MISSING, pres.Level.NO_DATA))
        per_second = bus.frames * 1000.0 / bus.window_ms if bus.window_ms else 0.0
        fields += [
            ("Ocupação (1 s)", _load_text(bus.load_percent)),
            ("Pico / máximo", f"{_decimal(bus.peak_percent, 1, '%')} / {_decimal(snapshot.max_load_percent, 1, '%')}"),
            ("Quadros por segundo", _decimal(per_second, 1)),
            ("Estado de erro", level_text(state_level, state_label, bold=state_level != pres.Level.NORMAL)),
        ]
    # Dados dos sensores sem fio: um identificador por módulo (0x380 + N), medido pela Probe a cada 5 s.
    for sample in sorted(snapshot.ids, key=lambda item: item.can_id):
        if not (WIRELESS_DATA_BASE < sample.can_id < WIRELESS_DATA_BASE + 0x20 and sample.window_ms):
            continue
        per_second = sample.count * 1000.0 / sample.window_ms
        bits_per_second = sample.bits * 1000.0 / sample.window_ms
        share = f" ({_decimal(100.0 * bits_per_second / snapshot.bitrate, 2, '%')} do barramento)" if snapshot.bitrate else ""
        fields.append(
            (
                f"Dados do Módulo {sample.can_id - WIRELESS_DATA_BASE:02d}",
                f"{_decimal(per_second, 1)} quadros/s, {_decimal(bits_per_second / 1000.0, 1, 'kbit/s')}{share}",
            )
        )
    coordination = snapshot.coordination
    fields += [
        ("Líder", f"Módulo {coordination.leader:02d}" if coordination.leader else pres.MISSING),
        ("Módulos em falha", ", ".join(f"{node:02d}" for node in sorted(coordination.nodes_in_fault)) or "Nenhum"),
    ]
    for link in snapshot.links:
        text = LINK_STATES.get(link.link_state, link.link_state.lower())
        if link.rtt_ms is not None:
            text += f", ida e volta {_decimal(link.rtt_ms, 1, 'ms')}"
        fields.append((f"Enlace {link.logical_id}", f"{text}, {link.telemetry_lost} perdidas"))
    return fields


def network_level(snapshot: MetricsSnapshot) -> pres.Level:
    bus = snapshot.bus
    if bus is None:
        return pres.Level.NO_DATA
    level = ERROR_STATES.get(snapshot.error_state, ("", pres.Level.NORMAL))[1]
    if bus.load_percent >= LOAD_CRITICAL:
        level = max(level, pres.Level.CRITICAL)
    elif bus.load_percent >= LOAD_ATTENTION or snapshot.coordination.nodes_in_fault:
        level = max(level, pres.Level.ATTENTION)
    return level


def nodes_fields(state: AppState) -> tuple[list[Field], pres.Level]:
    """Uma linha por módulo CAN e a pior condição entre eles."""

    fields: list[Field] = []
    worst = pres.Level.NO_DATA
    for node_id, node in sorted(state.nodes.items()):
        condition = pres.node_condition(node)
        worst = max(worst, condition.level)
        value = _condition_value(condition)
        role = "líder" if (node.role or "").upper() == "LEADER" else "seguidor"
        count = len(node.sensors)
        value.append(f"  {role}, {count} {'sensor' if count == 1 else 'sensores'}", style=palette.TEXT_SECONDARY)
        fields.append((f"Módulo {node_id:02d}", value))
    return fields or [("Módulos", "Nenhum módulo CAN conhecido nesta sessão")], worst


# ---------------------------------------------------------------------------
# Tela
# ---------------------------------------------------------------------------


class SupervisorPanel(VerticalScroll):
    """Um painel: campos e tendência de um sensor ou da rede."""

    def __init__(self, key: str) -> None:
        super().__init__(classes="section sup-panel")
        self.key = key

    def compose(self) -> ComposeResult:
        yield Fields()
        yield Label("", classes="trend-label")
        yield Sparkline([], classes="trend")

    def show(
        self, title: str, fields: list[Field], trend_label: str, trend: list[float], level: pres.Level = pres.Level.NORMAL
    ) -> None:
        self.border_title = title
        for item, css in LEVEL_CLASSES.items():
            self.set_class(item == level, css)
        try:
            self.query_one(Fields).set_fields(fields)
            label = self.query_one(Label)
            sparkline = self.query_one(Sparkline)
        except NoMatches:  # painel ainda sendo montado
            return
        label.update(trend_label)
        label.display = sparkline.display = bool(trend)
        sparkline.data = trend


class SupervisorScreen(BaseScreen):
    """Painéis lado a lado para acompanhar sensores e rede ao mesmo tempo."""

    BINDINGS = BaseScreen.BINDINGS + [
        Binding("a", "add", "Adicionar painel", show=False),
        Binding("x", "remove", "Remover painel", show=False),
        Binding("enter", "open", "Abrir", show=False),
        Binding("less_than_sign", "move(-1)", "Mover para trás", show=False),
        Binding("greater_than_sign", "move(1)", "Mover para frente", show=False),
    ]

    def key_hints(self) -> list[tuple[str, str, str | None]]:
        hints: list[tuple[str, str, str | None]] = [
            ("Esc", "Voltar", "screen.back"),
            ("a", "Adicionar", "screen.add"),
        ]
        if self.keys:
            hints += [
                ("x", "Remover", "screen.remove"),
                ("Enter", "Abrir tela", "screen.open"),
                ("Tab", "Próximo painel", None),
                ("< >", "Mover", None),
            ]
        hints.extend(self.tui.global_hints(home=False))
        return hints

    @property
    def keys(self) -> list[str]:
        return self.tui.preferences.supervisor_panels

    def compose_body(self) -> ComposeResult:
        with Container(id="sup-grid"):
            yield Static(
                f"Nenhum painel. Tecle a para adicionar até {PANEL_LIMIT}: a rede e os sensores, cada um uma vez.",
                id="sup-empty",
                classes="hint",
            )

    def breadcrumb(self) -> tuple[Sequence[str], str]:
        return ("Início", "Supervisório"), f"{len(self.keys)} de {PANEL_LIMIT} painéis"

    def on_mount(self) -> None:
        # Preferências editadas à mão: sem repetidos e no máximo cinco painéis.
        cleaned = [key for key in dict.fromkeys(self.keys) if valid_key(key)][:PANEL_LIMIT]
        if cleaned != self.keys:
            self.tui.set_preference("supervisor_panels", cleaned)
        super().on_mount()
        self.call_later(self._rebuild)

    async def _rebuild(self, focus: str | None = None) -> None:
        grid = self.query_one("#sup-grid", Container)
        await grid.query(SupervisorPanel).remove()
        panels = [SupervisorPanel(key) for key in self.keys]
        if panels:
            await grid.mount(*panels)
        self.query_one("#sup-empty").display = not panels
        self._apply_layout()
        self.query_one("#key-bar", KeyBar).set_hints(self.key_hints())
        self.tui.refresh_now()
        target = next((panel for panel in panels if panel.key == focus), panels[0] if panels else None)
        if target is not None:
            target.focus()

    def _apply_layout(self) -> None:
        panels = list(self.query(SupervisorPanel))
        columns, rows = grid_shape(
            len(panels), narrow=self.app.has_class("bp-narrow"), wide=self.app.has_class("bp-wide")
        )
        grid = self.query_one("#sup-grid", Container)
        grid.styles.grid_size_columns = columns
        grid.styles.grid_size_rows = rows
        for index, panel in enumerate(panels):
            last = index == len(panels) - 1
            panel.styles.column_span = columns * rows - len(panels) + 1 if last else 1

    def on_resize(self) -> None:
        # As classes de largura do app mudam no mesmo ciclo; aplica depois.
        self.call_after_refresh(self._apply_layout)

    def update_body(self, state: AppState) -> None:
        snapshot: MetricsSnapshot | None = None
        for panel in self.query(SupervisorPanel):
            if panel.key == NETWORK_PANEL:
                if snapshot is None:
                    self.tui.metrics.set_bitrate(state.gateway.arbitration_bitrate)
                    snapshot = self.tui.metrics.snapshot()
                trend = [sample.load_percent for sample in snapshot.bus_history[-120:]]
                panel.show(
                    "Rede e barramento", network_fields(snapshot), "Ocupação por segundo", trend, network_level(snapshot)
                )
                continue
            if panel.key == NODES_PANEL:
                fields, level = nodes_fields(state)
                panel.show("Módulos CAN", fields, "", [], level)
                continue
            ident = panel.key.partition(":")[2]
            sensor = find_sensor(state, ident)
            if sensor is None:
                panel.show(
                    f"Sensor {ident}",
                    [("Condição", level_text(pres.Level.NO_DATA, "Sensor ausente nesta sessão; aguardando dados"))],
                    "",
                    [],
                )
                continue
            label, trend = sensor_trend(sensor)
            title = f"Sensor {ident} · {pres.profile_label(sensor.profile_id)}"
            panel.show(title, sensor_fields(sensor), label, trend, pres.sensor_condition(sensor).level)

    # ------------------------------------------------------------------ ações

    def _focused_panel(self) -> SupervisorPanel | None:
        widget = self.focused
        while widget is not None and not isinstance(widget, SupervisorPanel):
            widget = widget.parent  # type: ignore[assignment]
        return widget

    def _save(self, keys: list[str], focus: str | None = None) -> None:
        self.tui.set_preference("supervisor_panels", keys)
        self.call_later(self._rebuild, focus)

    def action_add(self) -> None:
        if len(self.keys) >= PANEL_LIMIT:
            self.notify(f"O supervisório já tem {PANEL_LIMIT} painéis; remova um com x.", severity="warning")
            return
        options = panel_options(self.tui.state_store.snapshot(history=0, frames=0), self.keys)
        if not options:
            self.notify("Todos os sensores conhecidos e a rede já estão no supervisório.")
            return

        def chosen(key: str | None) -> None:
            if key and key not in self.keys and len(self.keys) < PANEL_LIMIT:
                self._save([*self.keys, key], focus=key)

        self.app.push_screen(
            ChoiceScreen("Adicionar painel", "Cada sensor entra uma vez só.", options), chosen
        )

    def action_remove(self) -> None:
        panel = self._focused_panel()
        if panel is None or panel.key not in self.keys:  # painel já saindo (teclas rápidas)
            return
        keys = [key for key in self.keys if key != panel.key]
        index = self.keys.index(panel.key)
        self._save(keys, focus=keys[min(index, len(keys) - 1)] if keys else None)

    def action_open(self) -> None:
        panel = self._focused_panel()
        if panel is None:
            return
        if panel.key == NETWORK_PANEL:
            self.app.push_screen(MetricsScreen())
        elif panel.key == NODES_PANEL:
            self.app.push_screen(NetworkScreen())
        else:
            self.tui.open_target(panel.key)

    def action_move(self, step: int) -> None:
        panel = self._focused_panel()
        if panel is None or panel.key not in self.keys:
            return
        keys = list(self.keys)
        index = keys.index(panel.key)
        target = index + step
        if 0 <= target < len(keys):
            keys[index], keys[target] = keys[target], keys[index]
            self._save(keys, focus=panel.key)
