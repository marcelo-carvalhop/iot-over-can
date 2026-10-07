"""Telas em tela cheia da TUI.

Navegação (Início é sempre a base da pilha):

    Início ─┬─ Módulo CAN NN ── Sensor NN.CC ── Espectro FFT
            ├─ Sensor NN.CC
            ├─ Comandos
            ├─ Rede CAN
            ├─ Mensagens
            ├─ Sensores sem fio
            └─ Ajuda

Cada tela recebe o estado pronto em ``refresh_view`` (chamado pelo app a cada
ciclo) e só atualiza os widgets que mudaram.
"""
from __future__ import annotations

import time
from collections.abc import Sequence
from typing import TYPE_CHECKING

from rich.text import Text
from textual import on
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.screen import Screen
from textual.widgets import Input, Label, OptionList, Sparkline, Static

from pico_tui import palette
from pico_tui import presentation as pres
from pico_tui.command_catalog import ACTIONS_BY_KEY, GROUP_ORDER, actions_for, can_command
from pico_tui.core.models import AppState, ConnectionMode, SensorNode
from pico_tui.spectrum import (  # noqa: F401 - reexportados por compatibilidade
    _spectrum_chart,
    _spectrum_metadata,
    _spectrum_peaks,
    spectrum_chart,
    spectrum_fields,
    spectrum_peaks,
)
from pico_tui.widgets import (
    LEVEL_COLORS,
    Breadcrumb,
    EquipmentList,
    Fields,
    FlowLine,
    KeyBar,
    MessageLog,
    Paragraphs,
    StableOptionList,
    StatusBand,
    hanging_lines,
    incident_items,
    level_text,
    node_title,
    sensor_prompt,
)

if TYPE_CHECKING:  # pragma: no cover
    from pico_tui.app import PicoTuiApp

# Reexportados para scripts antigos que importavam diálogos daqui.
from pico_tui.dialogs import (  # noqa: F401
    ConfigRequest,
    ConfigScreen,
    ConfirmScreen,
    ConnectionChoice,
    ConnectScreen,
    ParameterScreen,
    TargetScreen,
    WirelessNodeScreen,
)


def section(title: str, *, id: str | None = None, classes: str = "") -> Vertical:
    container = Vertical(id=id, classes=f"section {classes}".strip())
    container.border_title = title
    return container


def _value(text: str, color: str = palette.TEXT_PRIMARY, *, bold: bool = False) -> Text:
    return Text(text, style=f"bold {color}" if bold else color)


def _condition_value(condition: pres.Condition) -> Text:
    value = level_text(condition.level, bold=True)
    if condition.reasons:
        value.append(f"  {condition.reasons[0]}", style=palette.TEXT_PRIMARY)
        if len(condition.reasons) > 1:
            value.append(f" (+{len(condition.reasons) - 1})", style=palette.TEXT_MUTED)
    return value


# ---------------------------------------------------------------------------
# Base
# ---------------------------------------------------------------------------


class BaseScreen(Screen):
    """Faixa de estado, caminho de navegação, corpo e barra de teclas."""

    BINDINGS = [Binding("escape", "back", "Voltar", show=False)]
    SCREEN_HINTS: Sequence[tuple[str, str, str | None]] = ()
    SHOW_BACK = True

    @property
    def tui(self) -> PicoTuiApp:
        return self.app  # type: ignore[return-value]

    def compose(self) -> ComposeResult:
        yield StatusBand(id="status-band")
        yield Breadcrumb(id="breadcrumb")
        yield from self.compose_body()
        yield KeyBar(id="key-bar")

    def compose_body(self) -> ComposeResult:  # pragma: no cover - abstrato
        yield from ()

    def breadcrumb(self) -> tuple[Sequence[str], str]:
        return ("Início",), ""

    def key_hints(self) -> list[tuple[str, str, str | None]]:
        hints: list[tuple[str, str, str | None]] = []
        if self.SHOW_BACK:
            hints.append(("Esc", "Voltar", "screen.back"))
        hints.extend(self.SCREEN_HINTS)
        hints.extend(self.tui.global_hints(home=not self.SHOW_BACK))
        return hints

    def on_mount(self) -> None:
        parts, subtitle = self.breadcrumb()
        self.query_one("#breadcrumb", Breadcrumb).set_parts(parts, subtitle)
        self.query_one("#key-bar", KeyBar).set_hints(self.key_hints())
        self.call_after_refresh(self.tui.refresh_now)

    def on_screen_resume(self) -> None:
        self.call_after_refresh(self.tui.refresh_now)

    def refresh_view(self, state: AppState) -> None:
        self.query_one("#status-band", StatusBand).show(
            state,
            security_label=self.tui.security_label(),
            port=state.port,
            compact=self.app.has_class("bp-narrow") or self.app.has_class("bp-short"),
        )
        parts, subtitle = self.breadcrumb()
        self.query_one("#breadcrumb", Breadcrumb).set_parts(parts, subtitle)
        self.update_body(state)

    def update_body(self, state: AppState) -> None:  # pragma: no cover - abstrato
        pass

    def action_back(self) -> None:
        if len(self.app.screen_stack) > 1 and self.SHOW_BACK:
            self.app.pop_screen()


# ---------------------------------------------------------------------------
# Início
# ---------------------------------------------------------------------------


class HomeScreen(BaseScreen):
    SHOW_BACK = False

    def compose_body(self) -> ComposeResult:
        with VerticalScroll(id="home-body", classes="body"):
            with Horizontal(id="home-columns", classes="columns"):
                with Vertical(id="home-main", classes="column column-left column-main"):
                    with section("Precisa de atenção", id="sec-attention"):
                        yield StableOptionList(id="attention-list", empty="Nada requer atenção agora.")
                    with section("Equipamentos", id="sec-equipment", classes="grow"):
                        yield EquipmentList(
                            id="equipment-list",
                            empty="Nenhum equipamento ainda. Pressione F3 ou p para conectar.",
                        )
                with Vertical(id="home-side", classes="column column-side"):
                    with section("Rede CAN", id="sec-home-network"):
                        yield Fields(id="home-network")
                    with section("Conexão", id="sec-home-connection"):
                        yield Fields(id="home-connection")
            with section("Mensagens recentes", id="sec-home-messages"):
                yield MessageLog(id="home-message-log")

    def breadcrumb(self) -> tuple[Sequence[str], str]:
        return ("Início",), "visão geral do sistema"

    def on_mount(self) -> None:
        super().on_mount()
        self.set_messages_visible(self.tui.messages_on_home)
        self.query_one("#equipment-list", EquipmentList).focus()

    def set_messages_visible(self, visible: bool) -> None:
        panel = self.query_one("#sec-home-messages")
        panel.display = visible
        if visible:
            log = self.query_one("#home-message-log", MessageLog)
            log.clear()
            for entry in self.tui.visible_messages()[-200:]:
                log.add_entry(entry)

    def add_message(self, entry) -> None:
        panel = self.query_one("#sec-home-messages")
        if panel.display:
            self.query_one("#home-message-log", MessageLog).add_entry(entry)

    def update_body(self, state: AppState) -> None:
        self.query_one("#equipment-list", EquipmentList).show(state)

        attention = pres.attention_items(state)
        attention_list = self.query_one("#attention-list", StableOptionList)
        width = attention_list.prompt_width
        items = []
        for index, item in enumerate(attention[:20]):
            head = Text(f"{pres.symbol(item.level)} ", style=LEVEL_COLORS[item.level])
            head.append(item.title, style=f"bold {palette.TEXT_PRIMARY}")
            lines = hanging_lines([head, Text(item.reason, style=palette.TEXT_PRIMARY)], width, indent=2)
            items.append((f"{item.target}|{index}", Text("\n").join(lines)))
        attention_list.set_items(items)
        # A faixa de estado já diz "Operação normal"; a seção só aparece quando há o que tratar.
        self.query_one("#sec-attention").display = bool(items)

        network_section = self.query_one("#sec-home-network")
        network_section.display = state.connection_mode != ConnectionMode.SENSOR_DIRECT
        gateway, network = state.gateway, state.network
        bitrate = pres.MISSING
        if gateway.arbitration_bitrate:
            bitrate = f"{gateway.arbitration_bitrate // 1000} kbit/s"
            if gateway.data_bitrate:
                bitrate += f", dados {gateway.data_bitrate // 1000} kbit/s"
        bus = gateway.can_state if gateway.can_state != "UNKNOWN" else pres.MISSING
        bus_text = _value(bus)
        if network.bus_off:
            bus_text = level_text(pres.Level.CRITICAL, "Bus-off", bold=True)
        elif network.error_passive:
            bus_text = level_text(pres.Level.ATTENTION, "Error-passive", bold=True)
        self.query_one("#home-network", Fields).set_fields(
            [
                ("Barramento", bus_text),
                ("Taxa de bits", bitrate),
                ("Quadros", f"{pres.fmt_int(network.frames_rx)} recebidos, {pres.fmt_int(network.frames_tx)} enviados"),
                ("Erros", f"CRC {pres.fmt_int(network.crc_errors)}, interpretação {pres.fmt_int(network.parse_errors)}"),
            ]
        )
        associated = sum(len(node.sensors) for node in state.nodes.values())
        associated_uuids = {
            sensor.wireless_uuid.lower()
            for node in state.nodes.values()
            for sensor in node.sensors.values()
            if sensor.wireless_uuid and sensor.association_state.upper() != "UNBOUND"
        }
        now = time.monotonic()
        available_uuids = {
            candidate.wireless_uuid.lower()
            for node in state.nodes.values()
            for candidate in node.wireless_candidates.values()
            if candidate.wireless_uuid.lower() not in associated_uuids
            and (not candidate.last_seen_monotonic or now - candidate.last_seen_monotonic <= 15.0)
        }
        self.query_one("#home-connection", Fields).set_fields(
            [
                ("Estado", pres.connection_label(state.connection_state)),
                ("Equipamento", pres.mode_label(state.connection_mode)),
                ("Porta", state.port or pres.MISSING),
                ("Sensores sem fio", f"{len(available_uuids)} disponíveis, {associated} associados"),
                ("Última ação", state.last_action or pres.MISSING),
            ]
        )

    @on(OptionList.OptionSelected, "#equipment-list")
    @on(OptionList.OptionSelected, "#attention-list")
    def _open(self, event: OptionList.OptionSelected) -> None:
        option_id = str(event.option.id or "")
        if option_id and option_id != "__empty__":
            self.tui.open_target(option_id.split("|", 1)[0])


# ---------------------------------------------------------------------------
# Sensor
# ---------------------------------------------------------------------------


class SensorScreen(BaseScreen):
    """Tela própria de cada sensor: situação, métricas e intercorrências."""

    BINDINGS = BaseScreen.BINDINGS + [
        Binding("c", "commands", "Comandos", show=False),
        Binding("f", "spectrum", "Espectro", show=False),
        Binding("s", "status", "Atualizar", show=False),
        Binding("d", "unbind", "Desassociar", show=False),
        Binding("w", "wireless", "Sensores sem fio", show=False),
    ]

    def __init__(self, logical_id: str) -> None:
        super().__init__()
        self.logical_id = logical_id
        self.parent_id = int(logical_id.split(".", 1)[0])
        self._profile = ""
        self._wireless_associated = False  # associado, porém sem plano de dados
        self._wireless_bound = False       # associado a um módulo (com ou sem plano de dados)
        self._hints_profile: str | None = None

    def key_hints(self) -> list[tuple[str, str, str | None]]:
        hints: list[tuple[str, str, str | None]] = [("Esc", "Voltar", "screen.back")]
        if self._wireless_associated:
            hints.extend(
                [
                    ("d", "Desassociar", "screen.unbind"),
                    ("w", "Sensores sem fio", "screen.wireless"),
                ]
            )
        else:
            hints.append(("c", "Comandos", "screen.commands"))
            if self._profile == "VIBRATION":
                hints.append(("f", "Espectro FFT", "screen.spectrum"))
            hints.append(("s", "Atualizar", "screen.status"))
            if self._wireless_bound:
                hints.append(("w", "Sensores sem fio", "screen.wireless"))
        hints.extend(self.tui.global_hints(home=False, commands=False))
        return hints

    def compose_body(self) -> ComposeResult:
        with VerticalScroll(id="sensor-body", classes="body"), Horizontal(classes="columns"):
            with Vertical(classes="column"):
                with section("Situação"):
                    yield Fields(id="sensor-status")
                with section("Métricas de vibração", id="sec-metrics"):
                    yield Fields(id="sensor-metrics")
                    yield Label("Tendência do RMS", classes="trend-label")
                    yield Sparkline([], id="trend-rms", classes="trend")
                    yield Label("Tendência do PPV", classes="trend-label")
                    yield Sparkline([], id="trend-ppv", classes="trend")
                with section("Dados recebidos", id="sec-raw"):
                    yield Fields(id="sensor-raw")
            with Vertical(classes="column"):
                with section("Intercorrências", id="sec-incidents"):
                    yield Paragraphs(id="sensor-incidents", empty="Nenhuma intercorrência registrada nesta sessão.")
                    yield Fields(id="sensor-link", classes="subfields")
                with section("Configuração aplicada", id="sec-config"):
                    yield Fields(id="sensor-config")

    def breadcrumb(self) -> tuple[Sequence[str], str]:
        return ("Início", f"Módulo {self.parent_id:02d}", f"Sensor {self.logical_id}"), pres.profile_label(self._profile)

    def on_mount(self) -> None:
        super().on_mount()
        self.query_one("#sensor-body").focus()

    def update_body(self, state: AppState) -> None:
        node = state.nodes.get(self.parent_id)
        sensor = node.sensors.get(int(self.logical_id.split(".", 1)[1])) if node else None
        if sensor is None:
            self.query_one("#sensor-status", Fields).set_fields(
                [("Condição", level_text(pres.Level.NO_DATA, "Sensor não está mais presente na rede"))]
            )
            return
        profile = (sensor.profile_id or "").upper()
        wireless_associated = sensor.wireless_associated
        # Sem sessão autenticada com o módulo, a tela mostra o vínculo e
        # esconde ações que dependem do canal de dados.
        control_only = wireless_associated and not sensor.has_data_plane
        if (
            profile != self._profile
            or control_only != self._wireless_associated
            or wireless_associated != self._wireless_bound
        ):
            self._profile = profile
            self._wireless_associated = control_only
            self._wireless_bound = wireless_associated
            self.query_one("#key-bar", KeyBar).set_hints(self.key_hints())
        is_vibration = profile == "VIBRATION"
        self.query_one("#sec-metrics").display = is_vibration
        self.query_one("#sec-raw").display = not is_vibration

        self.query_one("#sensor-status", Fields).set_fields(self._status_fields(sensor))
        if is_vibration:
            self._update_vibration(sensor)
        else:
            raw = sensor.latest_telemetry.raw if sensor.latest_telemetry else {}
            fields = [(str(key).title(), str(value)) for key, value in sorted(raw.items())]
            self.query_one("#sensor-raw", Fields).set_fields(
                fields or [("Perfil", f"{pres.profile_label(profile)}: sem tela especializada; nenhum dado recebido ainda")]
            )

        self.query_one("#sensor-incidents", Paragraphs).set_items(incident_items(sensor.incidents, sensor.active_dtcs))
        if control_only and state.connection_mode == ConnectionMode.GATEWAY_CAN:
            self.query_one("#sensor-link", Fields).set_fields(
                [
                    ("Vínculo wireless", sensor.association_state.upper()),
                    ("Módulo responsável", f"Módulo {sensor.parent_node_id:02d}"),
                    ("RSSI do vínculo", f"{sensor.association_rssi_dbm} dBm" if sensor.association_rssi_dbm is not None else pres.MISSING),
                    ("Plano de dados", pres.data_link_label(sensor.data_link_state).capitalize()),
                ]
            )
            self.query_one("#sensor-config", Fields).set_fields(
                [
                    ("Configuração remota", "Disponível quando o plano de dados estiver com sessão autenticada"),
                    ("Gerenciar vínculo", "Use F7 ou w para associar/desassociar sensores"),
                ]
            )
        elif wireless_associated and state.connection_mode == ConnectionMode.GATEWAY_CAN:
            total = sensor.rx_count + sensor.lost_count
            self.query_one("#sensor-link", Fields).set_fields(
                [
                    ("Plano de dados", f"Sessão autenticada via Módulo {sensor.parent_node_id:02d}"),
                    ("Sinal Wi-Fi", f"{sensor.data_link_rssi_dbm} dBm" if sensor.data_link_rssi_dbm is not None else pres.MISSING),
                    ("Sessão ativa há", pres.fmt_int(sensor.data_link_session_age_s) + " s"),
                    ("Leituras no CAN", pres.fmt_int(sensor.rx_count)),
                    ("Perdidas no CAN", f"{pres.fmt_int(sensor.lost_count)} ({pres.fmt_number(sensor.loss_percent, 2, '%')})" if total else pres.MISSING),
                    ("Perdidas no Wi-Fi", pres.fmt_int(sensor.data_link_lost_datagrams)),
                    ("Rejeitadas (autenticação)", pres.fmt_int(sensor.data_link_auth_failures)),
                    ("Rejeitadas (repetição)", pres.fmt_int(sensor.data_link_replay_drops)),
                ]
            )
            self.query_one("#sensor-config", Fields).set_fields(self._config_fields(sensor))
        else:
            total = sensor.rx_count + sensor.lost_count
            self.query_one("#sensor-link", Fields).set_fields(
                [
                    ("Recebidas", pres.fmt_int(sensor.rx_count)),
                    ("Perdidas", f"{pres.fmt_int(sensor.lost_count)} ({pres.fmt_number(sensor.loss_percent, 2, '%')})" if total else pres.MISSING),
                    ("Duplicadas", pres.fmt_int(sensor.duplicate_count)),
                    ("Fora de ordem", pres.fmt_int(sensor.out_of_order_count)),
                ]
            )
            self.query_one("#sensor-config", Fields).set_fields(self._config_fields(sensor))

    @staticmethod
    def _config_fields(sensor: SensorNode) -> list[tuple[str, Text | str]]:
        cfg = sensor.configuration
        return [
            ("Modo", pres.sensor_mode_label(cfg.mode.value)),
            ("Taxa solicitada", pres.fmt_number(cfg.sample_rate_requested_hz, 1, "Hz")),
            ("Taxa efetiva", pres.fmt_number(cfg.sample_rate_effective_hz, 1, "Hz")),
            ("Janela", f"{(cfg.window_type or pres.MISSING).title()}, {cfg.window_size or pres.MISSING} amostras"),
            ("Limiar STA/LTA", pres.fmt_number(cfg.stalta_threshold, 2)),
            ("Ganho", pres.fmt_number(cfg.calibration_gain, 3)),
            ("Última alteração", _transaction_label(cfg.transaction_state)),
        ]

    @staticmethod
    def _status_fields(sensor: SensorNode) -> list[tuple[str, Text | str]]:
        sample = sensor.latest_telemetry
        health = sensor.health
        if health.telemetry_enabled:
            period = f", a cada {health.telemetry_period_ms} ms" if health.telemetry_period_ms else ""
            telemetry = f"Ligada{period}"
        else:
            telemetry = "Desligada"
        battery = "Não instrumentada"
        if sample is not None and sample.battery.valid:
            parts = []
            if sample.battery.percentage is not None:
                parts.append(f"{sample.battery.percentage:.0f}%")
            if sample.battery.voltage_v is not None:
                parts.append(pres.fmt_number(sample.battery.voltage_v, 2, "V"))
            battery = ", ".join(parts) or "Instrumentada"
        acquisition = sensor.acquisition_mode.value
        if acquisition == "POLLING":
            acquisition = "POLLING (modo oficial)"
        elif acquisition == "DRDY":
            acquisition = "DRDY (experimental)"
        elif acquisition == "UNKNOWN":
            acquisition = pres.MISSING
        if sensor.wireless_associated and not sensor.has_data_plane and sample is None:
            return [
                ("Condição", _condition_value(pres.sensor_condition(sensor))),
                ("Vínculo wireless", f"{sensor.association_state.upper()} via Módulo {sensor.parent_node_id:02d}"),
                ("RSSI do vínculo", f"{sensor.association_rssi_dbm} dBm" if sensor.association_rssi_dbm is not None else pres.MISSING),
                ("Plano de dados", pres.data_link_label(sensor.data_link_state).capitalize()),
                ("Telemetria", "Aguardando sessão autenticada com o módulo"),
                ("Perfil", pres.profile_label(sensor.profile_id)),
                ("Identificador", sensor.wireless_uuid or pres.MISSING),
            ]
        return [
            ("Condição", _condition_value(pres.sensor_condition(sensor))),
            ("Comunicação", f"{pres.status_label(sensor.status)}, última leitura {pres.fmt_age(pres.sensor_age(sensor))}"),
            ("Modo de operação", pres.sensor_mode_label(sensor.sensor_mode.value)),
            ("Aquisição", acquisition),
            ("Telemetria", telemetry),
            ("Qualidade do dado", pres.quality_label(sensor.quality)),
            ("Bateria", battery),
            ("Identificador", sensor.wireless_uuid or pres.MISSING),
        ]

    def _update_vibration(self, sensor: SensorNode) -> None:
        sample = sensor.latest_telemetry
        if sample is None:
            message = "Sem telemetria recebida."
            if sensor.wireless_associated and not sensor.has_data_plane:
                message += " O plano de dados via módulo CAN ainda não tem sessão autenticada."
            self.query_one("#sensor-metrics", Fields).set_fields([("Leituras", message)])
            return
        fft_off = sample.fft_valid is False

        def fft_metric(value: object, decimals: int, unit: str = "") -> str:
            return "Não calculada neste modo" if fft_off else pres.fmt_number(value, decimals, unit)

        clip = _value("Não")
        if sample.clipping is True:
            clip = level_text(pres.Level.ATTENTION, "Sim, sinal saturado", bold=True)
        elif sample.clipping is None:
            clip = _value(pres.MISSING)
        trigger = pres.MISSING if sample.stalta_triggered is None else ("Disparado" if sample.stalta_triggered else "Em repouso")
        self.query_one("#sensor-metrics", Fields).set_fields(
            [
                ("RMS", _value(pres.fmt_number(sample.rms, 4, sample.rms_unit), bold=True)),
                ("PPV", _value(pres.fmt_number(sample.ppv_mm_s, 3, "mm/s"), bold=True)),
                ("Frequência dominante", fft_metric(sample.peak_frequency_hz, 2, "Hz")),
                ("Amplitude dominante", fft_metric(sample.peak_amplitude, 5)),
                ("Curtose (excesso)", pres.fmt_number(sample.kurtosis, 3)),
                ("Fator de crista", pres.fmt_number(sample.crest_factor, 3)),
                ("Entropia espectral", fft_metric(sample.spectral_entropy, 3)),
                ("Gatilho STA/LTA", trigger),
                ("Saturação", clip),
                ("Eixo", sample.axis or pres.MISSING),
            ]
        )
        history = list(sensor.telemetry_history)[-120:]
        self.query_one("#trend-rms", Sparkline).data = [item.rms for item in history if item.rms is not None]
        self.query_one("#trend-ppv", Sparkline).data = [item.ppv_mm_s for item in history if item.ppv_mm_s is not None]

    def action_commands(self) -> None:
        self.tui.open_commands(f"sensor:{self.logical_id}")

    def action_spectrum(self) -> None:
        if self._profile == "VIBRATION":
            self.tui.start_action("sensor.fft", f"sensor:{self.logical_id}")

    def action_status(self) -> None:
        if not self._wireless_associated:
            self.tui.start_action("sensor.status", f"sensor:{self.logical_id}")

    def action_unbind(self) -> None:
        if self._wireless_associated:
            self.tui.request_wireless_unbind(self.logical_id)

    def action_wireless(self) -> None:
        self.tui.action_show_wireless()


def _transaction_label(value: str) -> str:
    return {
        "IDLE": "Nenhuma nesta sessão",
        "SENT": "Enviada, aguardando o sensor",
        "STAGED": "Recebida pelo sensor, aguardando aplicação",
        "QUEUED": "Na fila do sensor",
        "APPLIED": "Aplicada pelo sensor",
        "VERIFIED": "Aplicada e verificada",
        "FAILED": "Recusada pelo sensor",
        "REJECTED": "Recusada pelo sensor",
    }.get((value or "IDLE").upper(), value)


# ---------------------------------------------------------------------------
# Módulo CAN
# ---------------------------------------------------------------------------


class NodeScreen(BaseScreen):
    """Tela própria de cada módulo CAN físico."""

    BINDINGS = BaseScreen.BINDINGS + [
        Binding("c", "commands", "Comandos", show=False),
        Binding("s", "status", "Atualizar", show=False),
        Binding("w", "wireless", "Sensores sem fio", show=False),
    ]
    SCREEN_HINTS = (
        ("c", "Comandos", "screen.commands"),
        ("s", "Atualizar", "screen.status"),
        ("w", "Sensores sem fio", "screen.wireless"),
    )

    def __init__(self, node_id: int) -> None:
        super().__init__()
        self.node_id = node_id
        self._title = f"Módulo {node_id:02d}"

    def key_hints(self) -> list[tuple[str, str, str | None]]:
        hints: list[tuple[str, str, str | None]] = [("Esc", "Voltar", "screen.back"), *self.SCREEN_HINTS]
        hints.extend(self.tui.global_hints(home=False, commands=False))
        return hints

    def compose_body(self) -> ComposeResult:
        with VerticalScroll(id="node-body", classes="body"), Horizontal(classes="columns"):
            with Vertical(classes="column"):
                with section("Situação"):
                    yield Fields(id="node-status")
                with section("Sensor local", id="sec-local"):
                    yield Fields(id="node-local")
            with Vertical(classes="column"):
                with section("Sensores sem fio associados"):
                    yield StableOptionList(id="node-sensors", empty="Nenhum sensor sem fio associado a este módulo.")
                with section("Descoberta sem fio", id="sec-discovery"):
                    yield Fields(id="node-discovery")
                    yield Paragraphs(id="node-candidates", empty="Nenhum sensor anunciando por perto.")
                with section("Intercorrências"):
                    yield Paragraphs(id="node-incidents", empty="Nenhuma intercorrência registrada nesta sessão.")

    def breadcrumb(self) -> tuple[Sequence[str], str]:
        return ("Início", self._title), ""

    def on_mount(self) -> None:
        super().on_mount()
        self.query_one("#node-body").focus()

    def update_body(self, state: AppState) -> None:
        node = state.nodes.get(self.node_id)
        if node is None:
            self.query_one("#node-status", Fields).set_fields(
                [("Condição", level_text(pres.Level.NO_DATA, "Módulo não está mais presente na rede"))]
            )
            return
        self._title = node_title(node)
        uptime = pres.MISSING
        if node.uptime_ms:
            uptime = _duration(node.uptime_ms / 1000)
        caps = ", ".join(pres.capability_label(cap) for cap in sorted(node.capabilities)) or pres.MISSING
        self.query_one("#node-status", Fields).set_fields(
            [
                ("Condição", _condition_value(pres.node_condition(node))),
                ("Comunicação", f"{pres.status_label(node.status)}, último contato {pres.fmt_age(pres.node_age(node))}"),
                ("Papel na rede", pres.role_label(node.role)),
                ("Estado CAN", node.can_state if node.can_state != "UNKNOWN" else pres.MISSING),
                ("Capacidades", caps),
                ("Firmware", node.firmware_version or pres.MISSING),
                ("Protocolo", node.protocol_version or pres.MISSING),
                ("Tempo ligado", uptime),
                ("Quadros", f"{pres.fmt_int(node.rx_count)} recebidos, {pres.fmt_int(node.tx_count)} enviados, {pres.fmt_int(node.error_count)} erros"),
            ]
        )
        has_local = node.local_sensor_profile not in {"", "NONE"} or node.local_sensor_value is not None
        self.query_one("#sec-local").display = has_local
        if has_local:
            local_age = None
            if node.local_sensor_last_seen_monotonic:
                local_age = time.monotonic() - node.local_sensor_last_seen_monotonic
            self.query_one("#node-local", Fields).set_fields(
                [
                    ("Perfil", pres.profile_label(node.local_sensor_profile)),
                    ("Último valor", f"0x{node.local_sensor_value:02X}" if node.local_sensor_value is not None else pres.MISSING),
                    ("Publicação", pres.fmt_bool(node.local_sensor_enabled, "Ativa", "Desativada")),
                    ("Rodada", str(node.local_sensor_last_round) if node.local_sensor_last_round is not None else pres.MISSING),
                    ("Atualizado", pres.fmt_age(local_age)),
                ]
            )
        sensor_list = self.query_one("#node-sensors", StableOptionList)
        width = sensor_list.prompt_width
        sensor_list.set_items(
            [(f"sensor:{sensor.logical_id}", sensor_prompt(sensor, width, indent=0)) for _cid, sensor in sorted(node.sensors.items())]
        )
        self.query_one("#node-discovery", Fields).set_fields(
            [
                ("Varredura BLE", pres.discovery_label(node.wireless_discovery_state)),
                ("Ponto de acesso Wi-Fi", pres.discovery_label(node.wireless_ap_state)),
            ]
        )
        candidates = []
        for candidate in sorted(node.wireless_candidates.values(), key=lambda item: item.rssi_dbm, reverse=True):
            line = Text(candidate.wireless_uuid, style=f"bold {palette.TEXT_PRIMARY}")
            line.append(
                f"  {pres.profile_label(candidate.profile_id)}, sinal {candidate.rssi_dbm} dBm ({_rssi_label(candidate.rssi_dbm)})",
                style=palette.TEXT_SECONDARY,
            )
            candidates.append(line)
        self.query_one("#node-candidates", Paragraphs).set_items(candidates)
        self.query_one("#node-incidents", Paragraphs).set_items(incident_items(node.incidents, node.active_dtcs))

    @on(OptionList.OptionSelected, "#node-sensors")
    def _open_sensor(self, event: OptionList.OptionSelected) -> None:
        option_id = str(event.option.id or "")
        if option_id.startswith("sensor:"):
            self.tui.open_target(option_id)

    def action_commands(self) -> None:
        self.tui.open_commands(f"node:{self.node_id:02d}")

    def action_status(self) -> None:
        self.tui.start_action("node.status", f"node:{self.node_id:02d}")

    def action_wireless(self) -> None:
        self.tui.action_show_wireless()


def _rssi_label(rssi: int) -> str:
    if rssi >= -60:
        return "bom"
    if rssi >= -75:
        return "regular"
    return "fraco"


def _duration(seconds: float) -> str:
    seconds = int(seconds)
    days, rest = divmod(seconds, 86400)
    hours, rest = divmod(rest, 3600)
    minutes, secs = divmod(rest, 60)
    if days:
        return f"{days} d {hours} h"
    if hours:
        return f"{hours} h {minutes} min"
    if minutes:
        return f"{minutes} min {secs} s"
    return f"{secs} s"


# ---------------------------------------------------------------------------
# Comandos
# ---------------------------------------------------------------------------


class CommandScreen(BaseScreen):
    """Único lugar para enviar comandos: escolha o alvo e a ação."""

    BINDINGS = BaseScreen.BINDINGS + [
        Binding("a", "toggle_manual", "Comando manual", show=False),
        Binding("t", "focus_target", "Alvo", show=False),
    ]
    SCREEN_HINTS = (("Enter", "Executar", None), ("t", "Trocar alvo", "screen.focus_target"), ("a", "Comando manual", "screen.toggle_manual"))

    def __init__(self, target: str | None = None) -> None:
        super().__init__()
        self.target = target or ""
        self._target_options: tuple[tuple[str, str], ...] = ()
        self._actions_key: tuple = ()

    def key_hints(self) -> list[tuple[str, str, str | None]]:
        hints: list[tuple[str, str, str | None]] = [("Esc", "Voltar", "screen.back"), *self.SCREEN_HINTS]
        hints.extend(self.tui.global_hints(home=False, commands=False))
        return hints

    def compose_body(self) -> ComposeResult:
        with VerticalScroll(id="command-body", classes="body"):
            with Horizontal(classes="columns"):
                with Vertical(classes="column column-left column-main"):
                    with section("Ações", id="sec-actions", classes="grow"):
                        yield FlowLine(gap=3, id="cmd-target")
                        yield StableOptionList(id="cmd-actions", empty="Nenhuma ação disponível para este alvo.")
                with Vertical(classes="column column-side"):
                    with section("O que esta ação faz", id="sec-details"):
                        yield Paragraphs(id="cmd-details", empty="Selecione uma ação para ver os detalhes.")
                        yield Fields(id="cmd-result", classes="subfields")
            with section("Comando manual (avançado)", id="sec-manual"):
                yield Static(
                    "Texto enviado sem alteração ao equipamento conectado. Comece com : para comandos "
                    "internos da TUI, como :tel on ou :node 04.",
                    classes="hint",
                )
                yield Input(placeholder="ex.: STATUS, 22 20 FF 00 ou :tel on", id="cmd-manual")

    def breadcrumb(self) -> tuple[Sequence[str], str]:
        return ("Início", "Comandos"), ""

    def on_mount(self) -> None:
        super().on_mount()
        self.query_one("#sec-manual").display = False
        self.query_one("#cmd-actions", StableOptionList).focus()

    def _targets(self, state: AppState) -> list[tuple[str, str]]:
        targets: list[tuple[str, str]] = []
        if state.connection_mode != ConnectionMode.SENSOR_DIRECT:
            targets.append(("Rede CAN (todos os módulos)", "network"))
        for node_id, node in sorted(state.nodes.items()):
            if node.node_type != "DIRECT_SENSOR_HOST":
                targets.append((f"{node_title(node)} ({pres.role_label(node.role)})", f"node:{node_id:02d}"))
            for _cid, sensor in sorted(node.sensors.items()):
                targets.append((f"Sensor {sensor.logical_id} ({pres.profile_label(sensor.profile_id)})", f"sensor:{sensor.logical_id}"))
        if not targets:
            targets.append(("Rede CAN (todos os módulos)", "network"))
        return targets

    def update_body(self, state: AppState) -> None:
        targets = tuple(self._targets(state))
        self._target_options = targets
        values = [value for _label, value in targets]
        if self.target not in values:
            self.target = values[0]
        label = dict((value, text) for text, value in targets).get(self.target, self.target)
        target_line = Text("Alvo: ", style=palette.TEXT_MUTED)
        target_line.append(label, style=f"bold {palette.TEXT_PRIMARY}")
        change = Text("t", style=f"bold {palette.ACCENT_FOCUS}")
        change.append(" trocar alvo", style=palette.TEXT_SECONDARY)
        self.query_one("#cmd-target", FlowLine).set_segments([target_line, change], [None, "screen.focus_target"])
        self._update_actions(state)
        self.query_one("#cmd-result", Fields).set_fields([("Último envio", state.last_action or "Nenhum nesta sessão")])

    def _sensor_for(self, state: AppState) -> SensorNode | None:
        if not self.target.startswith("sensor:"):
            return None
        logical = self.target.split(":", 1)[1]
        parent, child = (int(part) for part in logical.split("."))
        node = state.nodes.get(parent)
        return node.sensors.get(child) if node else None

    def _update_actions(self, state: AppState) -> None:
        scope = self.target.split(":", 1)[0]
        profile = ""
        sensor = self._sensor_for(state)
        if sensor is not None:
            profile = sensor.profile_id
        mode = state.connection_mode.value
        wireless_without_data_plane = bool(
            sensor is not None
            and sensor.wireless_associated
            and not sensor.has_data_plane
            and state.connection_mode == ConnectionMode.GATEWAY_CAN
        )
        actions = [] if wireless_without_data_plane else actions_for(scope, profile=profile, mode=mode)
        key = (self.target, profile, mode, wireless_without_data_plane, tuple(action.key for action in actions))
        if key == self._actions_key:
            return
        self._actions_key = key
        items: list[tuple[str, Text]] = []
        for group in GROUP_ORDER:
            group_actions = [action for action in actions if action.group == group]
            if not group_actions:
                continue
            items.append((f"group:{group}", Text(group, style=f"bold {palette.TEXT_MUTED}")))
            for action in group_actions:
                prompt = Text("  ")
                prompt.append(action.title, style=palette.TEXT_PRIMARY)
                if action.confirm:
                    prompt.append("  pede confirmação", style=palette.TEXT_MUTED)
                items.append((action.key, prompt))
        option_list = self.query_one("#cmd-actions", StableOptionList)
        option_list.set_items(items)
        if wireless_without_data_plane:
            self.query_one("#cmd-details", Paragraphs).set_items(
                [
                    Text(
                        "Este sensor está associado, mas o plano de dados com o módulo CAN não está ativo: "
                        f"{pres.data_link_label(sensor.data_link_state)}.",
                        style=palette.TEXT_PRIMARY,
                    ),
                    Text("Telemetria, FFT e configuração voltam a ser oferecidas quando a sessão autenticada for estabelecida.", style=palette.TEXT_SECONDARY),
                    Text("Use F7 ou w para gerenciar associação e desassociação.", style=palette.ACCENT_FOCUS),
                ],
                indent=0,
            )
        else:
            self._show_details(option_list.highlighted_id)

    def _show_details(self, action_key: str | None) -> None:
        details = self.query_one("#cmd-details", Paragraphs)
        action = ACTIONS_BY_KEY.get(action_key or "")
        if action is None:
            details.set_items([])
            return
        lines = [Text(action.description, style=palette.TEXT_PRIMARY)]
        if action.scope in {"network", "node"} and action.key != "network.probe_status":
            node_id = int(self.target.split(":", 1)[1]) if self.target.startswith("node:") else None
            try:
                raw = can_command(action.key, node_id, action.parameter.default if action.parameter else None)
                suffix = ", conforme o valor escolhido" if action.parameter else ""
                lines.append(Text(f"Envia {raw}{suffix}", style=palette.TEXT_MUTED))
            except ValueError:
                pass
        notes = []
        if action.protected:
            notes.append(f"exige autorização do operador (segurança {self.tui.security_label()})")
        if action.confirm:
            notes.append("pede confirmação antes de enviar")
        if notes:
            lines.append(Text("Esta ação " + " e ".join(notes) + ".", style=palette.TEXT_MUTED))
        details.set_items(lines, indent=0)

    def set_target(self, target: str) -> None:
        if target != self.target:
            self.target = target
            self._actions_key = ()
            self.tui.refresh_now()

    @on(OptionList.OptionHighlighted, "#cmd-actions")
    def _highlighted(self, event: OptionList.OptionHighlighted) -> None:
        self._show_details(str(event.option.id or ""))

    @on(OptionList.OptionSelected, "#cmd-actions")
    def _selected(self, event: OptionList.OptionSelected) -> None:
        action_key = str(event.option.id or "")
        if action_key in ACTIONS_BY_KEY:
            self.tui.start_action(action_key, self.target)

    @on(Input.Submitted, "#cmd-manual")
    def _manual(self, event: Input.Submitted) -> None:
        text = event.value.strip()
        event.input.value = ""
        if text:
            self.tui.submit_manual_command(text)

    def action_toggle_manual(self) -> None:
        manual = self.query_one("#sec-manual")
        manual.display = not manual.display
        if manual.display:
            self.query_one("#cmd-manual", Input).focus()
        else:
            self.query_one("#cmd-actions", StableOptionList).focus()

    def action_focus_target(self) -> None:
        def chosen(value: str | None) -> None:
            if value:
                self.set_target(value)
            self.query_one("#cmd-actions", StableOptionList).focus()

        self.app.push_screen(TargetScreen(list(self._target_options), self.target), chosen)

    def action_back(self) -> None:
        manual = self.query_one("#sec-manual")
        if manual.display and self.focused is self.query_one("#cmd-manual", Input):
            self.action_toggle_manual()
            return
        super().action_back()


# ---------------------------------------------------------------------------
# Sensores sem fio
# ---------------------------------------------------------------------------


class WirelessScreen(BaseScreen):
    """Gerencia descoberta, associação e remoção de vínculos wireless."""

    BINDINGS = BaseScreen.BINDINGS + [
        Binding("a", "associate", "Associar", show=False),
        Binding("d", "unbind", "Desassociar", show=False),
    ]

    def __init__(self) -> None:
        super().__init__()
        self._state: AppState | None = None
        self._available: dict[str, list[tuple[int, object]]] = {}
        self._associated: dict[str, SensorNode] = {}

    def key_hints(self) -> list[tuple[str, str, str | None]]:
        hints = [
            ("Esc", "Voltar", "screen.back"),
            ("Enter/a", "Associar disponível", "screen.associate"),
            ("d", "Desassociar vínculo", "screen.unbind"),
        ]
        hints.extend(self.tui.global_hints(home=False, commands=False))
        return hints

    def compose_body(self) -> ComposeResult:
        with VerticalScroll(id="wireless-body", classes="body"):
            yield Static(
                "A descoberta BLE é contínua. Associar define qual módulo CAN será responsável pelo sensor; "
                "desassociar remove apenas esse vínculo lógico.",
                classes="hint",
            )
            with Horizontal(classes="columns"):
                with Vertical(classes="column column-left column-main"):
                    with section("Disponíveis", id="sec-wireless-available", classes="grow"):
                        yield StableOptionList(
                            id="wireless-available",
                            empty="Nenhum sensor sem fio disponível.\nObservações dos últimos 15 s.",
                        )
                with Vertical(classes="column column-side"):
                    with section("Associados", id="sec-wireless-associated", classes="grow"):
                        yield StableOptionList(id="wireless-associated", empty="Nenhum sensor sem fio associado.")
            with section("Detalhes", id="sec-wireless-details"):
                yield Paragraphs(id="wireless-details", empty="Realce um sensor para ver detalhes e recepção por módulo.")

    def breadcrumb(self) -> tuple[Sequence[str], str]:
        return ("Início", "Sensores sem fio"), "descoberta e vínculos"

    def on_mount(self) -> None:
        super().on_mount()
        self.query_one("#wireless-available", StableOptionList).focus()

    @staticmethod
    def _inventory(state: AppState) -> tuple[dict[str, list[tuple[int, object]]], dict[str, SensorNode]]:
        associated: dict[str, SensorNode] = {}
        for node in state.nodes.values():
            for sensor in node.sensors.values():
                if sensor.wireless_uuid and sensor.association_state.upper() != "UNBOUND":
                    associated[sensor.wireless_uuid.lower()] = sensor
        now = time.monotonic()
        available: dict[str, list[tuple[int, object]]] = {}
        for node_id, node in state.nodes.items():
            for candidate in node.wireless_candidates.values():
                key = candidate.wireless_uuid.lower()
                if key in associated:
                    continue
                if candidate.last_seen_monotonic and now - candidate.last_seen_monotonic > 15.0:
                    continue
                available.setdefault(key, []).append((node_id, candidate))
        for observations in available.values():
            observations.sort(key=lambda item: item[1].rssi_dbm, reverse=True)
        return available, associated

    def update_body(self, state: AppState) -> None:
        self._state = state
        self._available, self._associated = self._inventory(state)
        available_list = self.query_one("#wireless-available", StableOptionList)
        available_items: list[tuple[str, Text]] = []
        for key, observations in sorted(self._available.items(), key=lambda item: item[1][0][1].rssi_dbm, reverse=True):
            best_node, best = observations[0]
            # A lista wireless não depende da largura transitória da OptionList.
            # Cada informação ocupa uma linha semântica curta; assim o primeiro
            # refresh já é seguro no terminal mínimo suportado (48 colunas).
            lines = [
                Text(f"  {best.wireless_uuid}", style=f"bold {palette.TEXT_PRIMARY}"),
                Text(f"  {pres.profile_label(best.profile_id)}", style=palette.TEXT_SECONDARY),
                Text(
                    f"  Melhor: Módulo {best_node:02d} · {best.rssi_dbm} dBm ({_rssi_label(best.rssi_dbm)})",
                    style=palette.TEXT_SECONDARY,
                ),
                Text(f"  Observado por {len(observations)} módulo(s)", style=palette.TEXT_MUTED),
            ]
            available_items.append((f"candidate:{key}", Text("\n").join(lines)))
        available_list.set_items(available_items)

        assoc_list = self.query_one("#wireless-associated", StableOptionList)
        assoc_items: list[tuple[str, Text]] = []
        for key, sensor in sorted(self._associated.items(), key=lambda item: item[1].logical_id):
            rssi = f"{sensor.association_rssi_dbm} dBm" if sensor.association_rssi_dbm is not None else pres.MISSING
            lines = [
                Text(f"  Sensor {sensor.logical_id}", style=f"bold {palette.TEXT_PRIMARY}"),
                Text(
                    f"  {pres.profile_label(sensor.profile_id)} · {sensor.association_state.upper()}",
                    style=palette.TEXT_SECONDARY,
                ),
                Text(f"  {sensor.wireless_uuid}", style=palette.TEXT_MUTED),
                Text(f"  Módulo {sensor.parent_node_id:02d} · {rssi}", style=palette.TEXT_SECONDARY),
            ]
            assoc_items.append((f"sensor:{sensor.logical_id}", Text("\n").join(lines)))
        assoc_list.set_items(assoc_items)
        self._show_current_details()

    def _focused_id(self) -> str | None:
        associated = self.query_one("#wireless-associated", StableOptionList)
        available = self.query_one("#wireless-available", StableOptionList)
        if self.focused is associated:
            return associated.highlighted_id
        if self.focused is available:
            return available.highlighted_id
        return available.highlighted_id or associated.highlighted_id

    def _show_current_details(self) -> None:
        option_id = self._focused_id()
        details = self.query_one("#wireless-details", Paragraphs)
        if not option_id:
            details.set_items([])
            return
        if option_id.startswith("candidate:"):
            key = option_id.split(":", 1)[1]
            observations = self._available.get(key, [])
            if not observations:
                details.set_items([])
                return
            best_node, best = observations[0]
            lines = [
                Text(f"{best.wireless_uuid} · {pres.profile_label(best.profile_id)} · protocolo {best.protocol_version or pres.MISSING}", style=palette.TEXT_PRIMARY),
                Text(f"Recomendação atual: Módulo {best_node:02d} ({best.rssi_dbm} dBm). A escolha não é automática.", style=palette.ACCENT_FOCUS),
            ]
            for node_id, candidate in observations:
                lines.append(Text(f"Módulo {node_id:02d}: {candidate.rssi_dbm} dBm ({_rssi_label(candidate.rssi_dbm)})", style=palette.TEXT_SECONDARY))
            details.set_items(lines, indent=2)
            return
        if option_id.startswith("sensor:"):
            logical_id = option_id.split(":", 1)[1]
            sensor = next((item for item in self._associated.values() if item.logical_id == logical_id), None)
            if sensor is None:
                details.set_items([])
                return
            lines = [
                Text(f"Sensor {sensor.logical_id} · {pres.profile_label(sensor.profile_id)}", style=f"bold {palette.TEXT_PRIMARY}"),
                Text(f"UUID {sensor.wireless_uuid}", style=palette.TEXT_SECONDARY),
                Text(f"Vínculo {sensor.association_state.upper()} · Módulo {sensor.parent_node_id:02d}", style=palette.TEXT_SECONDARY),
                Text(f"Plano de dados: {pres.data_link_label(sensor.data_link_state)}", style=palette.TEXT_SECONDARY),
                Text(
                    "O vínculo define o módulo responsável. Telemetria e comandos passam por ele depois que o sensor é autenticado.",
                    style=palette.TEXT_MUTED,
                ),
            ]
            details.set_items(lines, indent=2)

    @on(OptionList.OptionHighlighted, "#wireless-available")
    @on(OptionList.OptionHighlighted, "#wireless-associated")
    def _highlighted(self) -> None:
        self._show_current_details()

    @on(OptionList.OptionSelected, "#wireless-available")
    def _candidate_selected(self, event: OptionList.OptionSelected) -> None:
        option_id = str(event.option.id or "")
        if option_id.startswith("candidate:"):
            observations = self._available.get(option_id.split(":", 1)[1], [])
            if observations:
                self.tui.request_wireless_bind(observations[0][1].wireless_uuid)

    @on(OptionList.OptionSelected, "#wireless-associated")
    def _associated_selected(self, event: OptionList.OptionSelected) -> None:
        option_id = str(event.option.id or "")
        if option_id.startswith("sensor:"):
            self.tui.open_target(option_id)

    def action_associate(self) -> None:
        available = self.query_one("#wireless-available", StableOptionList)
        option_id = available.highlighted_id
        if option_id and option_id.startswith("candidate:"):
            observations = self._available.get(option_id.split(":", 1)[1], [])
            if observations:
                self.tui.request_wireless_bind(observations[0][1].wireless_uuid)
                return
        self.notify("Realce um sensor na lista Disponíveis.", severity="warning", timeout=3)

    def action_unbind(self) -> None:
        associated = self.query_one("#wireless-associated", StableOptionList)
        option_id = associated.highlighted_id
        if option_id and option_id.startswith("sensor:"):
            self.tui.request_wireless_unbind(option_id.split(":", 1)[1])
            return
        self.notify("Realce um sensor na lista Associados.", severity="warning", timeout=3)


# ---------------------------------------------------------------------------
# Rede CAN
# ---------------------------------------------------------------------------


class NetworkScreen(BaseScreen):
    BINDINGS = BaseScreen.BINDINGS + [Binding("c", "commands", "Comandos", show=False)]
    SCREEN_HINTS = (("c", "Comandos da rede", "screen.commands"),)

    def key_hints(self) -> list[tuple[str, str, str | None]]:
        hints: list[tuple[str, str, str | None]] = [("Esc", "Voltar", "screen.back"), *self.SCREEN_HINTS]
        hints.extend(self.tui.global_hints(home=False, commands=False))
        return hints

    def compose_body(self) -> ComposeResult:
        with VerticalScroll(id="network-body", classes="body"), Horizontal(classes="columns"):
            with Vertical(classes="column column-left"):
                with section("Barramento"):
                    yield Fields(id="net-bus")
                with section("Tráfego"):
                    yield Fields(id="net-traffic")
                with section("Probe 00 (instrumentação)"):
                    yield Fields(id="net-probe")
            with Vertical(classes="column"), section("Quadros recentes", id="sec-frames"):
                yield Paragraphs(id="net-frames", empty="Nenhum quadro CAN recebido nesta sessão.")

    def breadcrumb(self) -> tuple[Sequence[str], str]:
        return ("Início", "Rede CAN"), ""

    def update_body(self, state: AppState) -> None:
        gateway, network = state.gateway, state.network

        def bits(value: int | None) -> str:
            return f"{value // 1000} kbit/s" if value else pres.MISSING

        def flag(value: bool, label: str, level: pres.Level) -> Text:
            return level_text(level, label, bold=True) if value else _value("Não")

        self.query_one("#net-bus", Fields).set_fields(
            [
                ("Estado", gateway.can_state if gateway.can_state != "UNKNOWN" else pres.MISSING),
                ("Arbitragem", bits(gateway.arbitration_bitrate)),
                ("Fase de dados", bits(gateway.data_bitrate)),
                ("Bus-off", flag(network.bus_off, "Sim", pres.Level.CRITICAL)),
                ("Error-passive", flag(network.error_passive, "Sim", pres.Level.ATTENTION)),
                ("Utilização", pres.fmt_number(network.utilization_percent, 1, "%")),
            ]
        )
        self.query_one("#net-traffic", Fields).set_fields(
            [
                ("Quadros recebidos", pres.fmt_int(network.frames_rx)),
                ("Quadros enviados", pres.fmt_int(network.frames_tx)),
                ("Bytes recebidos", pres.fmt_int(network.bytes_rx)),
                ("Bytes enviados", pres.fmt_int(network.bytes_tx)),
                ("Erros de CRC", pres.fmt_int(network.crc_errors)),
                ("Erros de interpretação", pres.fmt_int(network.parse_errors)),
                ("Transferências em curso", pres.fmt_int(network.active_transfers)),
            ]
        )
        uptime = _duration(gateway.uptime_ms / 1000) if gateway.uptime_ms else pres.MISSING
        self.query_one("#net-probe", Fields).set_fields(
            [
                ("Firmware", gateway.firmware_version or pres.MISSING),
                ("Protocolo", gateway.protocol_version or pres.MISSING),
                ("Porta serial", gateway.serial_port or state.port or pres.MISSING),
                ("Wi-Fi", gateway.wifi_state if gateway.wifi_state != "UNKNOWN" else pres.MISSING),
                ("Tempo ligado", uptime),
            ]
        )
        frames = []
        for frame in list(network.recent_frames)[::-1][:40]:
            line = Text(f"{'Recebido' if frame.direction == 'RX' else 'Enviado'} ", style=palette.TEXT_MUTED)
            line.append(f"0x{frame.can_id:08X}", style=f"bold {palette.TEXT_PRIMARY}")
            line.append(f"  {'FD' if frame.fd else 'clássico'}, {len(frame.data)} bytes  ", style=palette.TEXT_SECONDARY)
            line.append(frame.data.hex(" ") or "(vazio)", style=palette.TEXT_PRIMARY)
            frames.append(line)
        self.query_one("#net-frames", Paragraphs).set_items(frames, indent=4)

    def action_commands(self) -> None:
        self.tui.open_commands("network")


# ---------------------------------------------------------------------------
# Mensagens
# ---------------------------------------------------------------------------


class MessagesScreen(BaseScreen):
    BINDINGS = BaseScreen.BINDINGS + [
        Binding("d", "toggle_debug", "Depuração", show=False),
        Binding("h", "toggle_home", "Na tela inicial", show=False),
        Binding("l", "clear_view", "Limpar", show=False),
    ]

    def key_hints(self) -> list[tuple[str, str, str | None]]:
        debug = "ocultar depuração" if self.tui.preferences.show_debug_messages else "mostrar depuração"
        home = "tirar da tela inicial" if self.tui.messages_on_home else "fixar na tela inicial"
        hints: list[tuple[str, str, str | None]] = [
            ("Esc", "Voltar", "screen.back"),
            ("d", debug.capitalize(), "screen.toggle_debug"),
            ("h", home.capitalize(), "screen.toggle_home"),
            ("l", "Limpar visualização", "screen.clear_view"),
        ]
        hints.extend(self.tui.global_hints(home=False, commands=True))
        return hints

    def compose_body(self) -> ComposeResult:
        with section("Todas as mensagens", id="sec-messages", classes="body grow"):
            yield MessageLog(id="messages-log")

    def breadcrumb(self) -> tuple[Sequence[str], str]:
        return ("Início", "Mensagens"), "registro completo também salvo em logs/"

    def on_mount(self) -> None:
        super().on_mount()
        self._reload()
        self.query_one("#messages-log", MessageLog).focus()

    def _reload(self) -> None:
        log = self.query_one("#messages-log", MessageLog)
        log.clear()
        for entry in self.tui.visible_messages():
            log.add_entry(entry)

    def add_message(self, entry) -> None:
        if entry.level == "DEBUG" and not self.tui.preferences.show_debug_messages:
            return
        self.query_one("#messages-log", MessageLog).add_entry(entry)

    def action_toggle_debug(self) -> None:
        self.tui.set_preference("show_debug_messages", not self.tui.preferences.show_debug_messages)
        self._reload()
        self.query_one("#key-bar", KeyBar).set_hints(self.key_hints())

    def action_toggle_home(self) -> None:
        self.tui.set_messages_on_home(not self.tui.messages_on_home)
        self.query_one("#key-bar", KeyBar).set_hints(self.key_hints())
        state = "aparecem" if self.tui.messages_on_home else "não aparecem mais"
        self.notify(f"As mensagens recentes {state} na tela inicial.", timeout=3)

    def action_clear_view(self) -> None:
        self.query_one("#messages-log", MessageLog).clear()


# ---------------------------------------------------------------------------
# Espectro FFT
# ---------------------------------------------------------------------------


class FftScreen(BaseScreen):
    BINDINGS = BaseScreen.BINDINGS + [Binding("n", "again", "Novo espectro", show=False)]

    def __init__(self, logical_id: str, *, requested_at: float | None = None) -> None:
        super().__init__()
        self.logical_id = logical_id
        self.requested_at = requested_at or 0.0
        self._chart_key: tuple = ()

    def key_hints(self) -> list[tuple[str, str, str | None]]:
        hints: list[tuple[str, str, str | None]] = [("Esc", "Voltar", "screen.back"), ("n", "Novo espectro", "screen.again")]
        hints.extend(self.tui.global_hints(home=False, commands=True))
        return hints

    def compose_body(self) -> ComposeResult:
        with VerticalScroll(id="fft-body", classes="body"):
            with section("Espectro", id="sec-chart"):
                yield Static("Aguardando o espectro do sensor...", id="fft-ascii")
            with Horizontal(classes="columns"):
                with Vertical(classes="column"), section("Picos principais"):
                    yield Paragraphs(id="fft-peaks", empty="Sem picos para mostrar.")
                with Vertical(classes="column"), section("Parâmetros"):
                    yield Fields(id="fft-meta")

    def breadcrumb(self) -> tuple[Sequence[str], str]:
        parent = int(self.logical_id.split(".", 1)[0])
        return ("Início", f"Módulo {parent:02d}", f"Sensor {self.logical_id}", "Espectro"), ""

    def update_body(self, state: AppState) -> None:
        parent, child = (int(part) for part in self.logical_id.split("."))
        node = state.nodes.get(parent)
        sensor = node.sensors.get(child) if node else None
        chart = self.query_one("#fft-ascii", Static)
        if sensor is None:
            chart.update("Sensor não encontrado.")
            return
        telemetry = sensor.latest_telemetry
        if telemetry is not None and telemetry.fft_valid is False:
            chart.update("O sensor não calcula FFT no modo atual (esperado no modo Sísmico).")
            return
        spectrum = sensor.latest_fft
        if spectrum is None or spectrum.received_wall_time < self.requested_at:
            chart.update("Aguardando o espectro do sensor. O vetor chega separado da telemetria, após o pedido.")
            return
        width = max(20, self.query_one("#sec-chart").content_size.width or 60)
        key = (spectrum.received_wall_time, width)
        if key != self._chart_key:
            self._chart_key = key
            chart.update(Text(spectrum_chart(spectrum, width=width), style=palette.ACCENT_FOCUS))
        self.query_one("#fft-peaks", Paragraphs).set_items([Text(row) for row in spectrum_peaks(spectrum)])
        self.query_one("#fft-meta", Fields).set_fields(spectrum_fields(spectrum))

    def on_resize(self) -> None:
        self._chart_key = ()

    def action_again(self) -> None:
        self.tui.start_action("sensor.fft", f"sensor:{self.logical_id}")


# ---------------------------------------------------------------------------
# Ajuda
# ---------------------------------------------------------------------------


class HelpScreen(BaseScreen):
    def key_hints(self) -> list[tuple[str, str, str | None]]:
        return [("Esc", "Voltar", "screen.back"), *self.tui.global_hints(home=False, commands=True, help=False)]

    def compose_body(self) -> ComposeResult:
        with VerticalScroll(id="help-body", classes="body"):
            with section("Navegação"):
                yield Fields(
                    [
                        ("Setas e Enter", "Escolher um item e abrir"),
                        ("Esc", "Voltar para a tela anterior"),
                        ("F2 ou i", "Início, com o resumo do sistema"),
                        ("F3 ou p", "Conectar ou trocar a porta serial"),
                        ("F4 ou c", "Comandos (na tela de um equipamento, já com ele como alvo)"),
                        ("F5 ou r", "Rede CAN: barramento, tráfego e quadros"),
                        ("F6 ou m", "Mensagens: registro completo de eventos"),
                        ("F7 ou w", "Sensores sem fio: descobrir, associar e desassociar"),
                        ("F1 ou ?", "Esta ajuda"),
                        ("F10 ou q", "Sair"),
                        ("Tab", "Passar para a próxima lista ou campo"),
                        ("Ctrl+C", "Interromper a telemetria do sensor direto"),
                    ]
                )
            with section("Símbolos de condição"):
                yield Fields(
                    [
                        (f"{pres.symbol(pres.Level.NORMAL)} Normal", "Equipamento comunicando, sem diagnósticos ativos"),
                        (f"{pres.symbol(pres.Level.ATTENTION)} Atenção", "Sem dados recentes, diagnóstico de aviso ou sinal saturado"),
                        (f"{pres.symbol(pres.Level.CRITICAL)} Crítico", "Comunicação perdida, diagnóstico crítico ou bus-off"),
                        (f"{pres.symbol(pres.Level.NO_DATA)} Sem dados", "Aguardando a primeira comunicação"),
                    ]
                )
            with section("Telas de equipamento"):
                yield Paragraphs(id="help-screens")
            with section("Comandos internos (avançado)"):
                yield Paragraphs(id="help-internal")

    def breadcrumb(self) -> tuple[Sequence[str], str]:
        return ("Início", "Ajuda"), ""

    def on_mount(self) -> None:
        super().on_mount()
        self.query_one("#help-screens", Paragraphs).set_items(
            indent=0,
            items=[
                Text("Na tela inicial, Enter sobre um módulo ou sensor abre a tela própria dele."),
                Text("A tela do sensor mostra situação, métricas do perfil, intercorrências e a configuração aplicada."),
                Text("A tela do módulo mostra papel na rede, sensor local, sensores sem fio e descoberta BLE."),
                Text("F7 ou w abre o gerenciamento wireless: candidatos disponíveis, vínculos ativos, associação e desassociação."),
                Text("Sensores wireless associados expõem telemetria, FFT e configuração pelo módulo CAN quando o plano de dados tem sessão autenticada; sem ela, essas ações ficam ocultas."),
                Text("Em qualquer tela de equipamento com canal de comandos disponível, c abre os comandos já apontados para ele."),
            ]
        )
        self.query_one("#help-internal", Paragraphs).set_items(
            [
                Text("Na tela de comandos, a abre o campo de comando manual. Texto sem : vai direto ao equipamento."),
                Text(":status   :node 04   :node 04.01   :can   :can 22 20 FF 00   :election   :canstatus"),
                Text(":tel on|off|once|fast|slow|period <ms>   :fft [sensor] bins=64   :acq polling"),
                Text(":dtc list [sensor]   :dtc clear [sensor] all|0xCÓDIGO   :wifi on|off|status"),
                Text(":config [sensor] mode=STRUCTURAL rate=250 window=HANN   :connect [porta] [modo]"),
                Text(":wireless   :bind [uuid] [módulo]   :unbind [uuid] [módulo]"),
                Text(":disconnect   :reconnect   :export csv   :snapshot   :security   :lock   :quit"),
            ],
            indent=2,
        )
