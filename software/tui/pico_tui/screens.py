from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Any, Callable

from textual import on
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.message import Message
from textual.screen import ModalScreen
from textual.widgets import Button, DataTable, Input, Label, OptionList, Sparkline, Static
from textual.widgets.option_list import Option

from pico_tui import commands
from pico_tui import palette
from pico_tui.core.models import AppState, SensorNode, SpectrumSample
from pico_tui.dtc_catalog import dtc_description
from pico_tui.serial_client import list_available_ports


@dataclass(slots=True)
class ConnectionChoice:
    port: str = ""
    mode: str = "auto"
    demo: bool = False


@dataclass(slots=True)
class ConfigRequest:
    mode: str
    window: str
    rate_hz: float | None
    window_size: int | None
    stalta: float | None
    gain: float | None
    apply_and_verify: bool




class TelemetryCommandRequested(Message):
    """Ação solicitada pela tela detalhada de telemetria."""

    def __init__(self, logical_id: str, action: str, value: int | None = None) -> None:
        self.logical_id = logical_id
        self.action = action.upper()
        self.value = value
        super().__init__()


@dataclass(slots=True)
class FftRequest:
    bins: int
    mode: str


class ConnectScreen(ModalScreen[ConnectionChoice | None]):
    BINDINGS = [
        Binding("r", "refresh_ports", "Atualizar", show=True),
        Binding("escape", "cancel", "Sair", show=True),
        Binding("d", "demo", "Demonstração", show=True),
    ]

    def compose(self) -> ComposeResult:
        with Vertical(id="connect-dialog", classes="modal"):
            yield Static("CONEXÃO", classes="modal-title")
            yield Label("Porta serial")
            yield OptionList(id="port-list")
            yield Label("Modo de protocolo")
            yield OptionList(
                Option("Detecção automática", id="auto"),
                Option("Gateway CAN FD", id="gateway"),
                Option("Sensor direto Pico 2 W", id="sensor"),
                id="connection-mode-list",
            )
            yield Label("Caminho manual")
            yield Input(placeholder="/dev/ttyACM0, /dev/ttyUSB0 ou COM5", id="manual-port")
            with Horizontal(classes="modal-buttons"):
                yield Button("Demonstração", id="demo-button")
                yield Button("Conectar", id="connect-button", variant="primary")
                yield Button("Sair", id="cancel-button")
            yield Static("R atualiza • D inicia demonstração • Esc sai", classes="modal-hint")

    def on_mount(self) -> None:
        self._populate_ports()
        mode_list = self.query_one("#connection-mode-list", OptionList)
        mode_list.highlighted = 0
        self.query_one("#port-list", OptionList).focus()

    def _populate_ports(self) -> None:
        widget = self.query_one("#port-list", OptionList)
        widget.clear_options()
        ports = list_available_ports()
        if not ports:
            widget.add_option(Option("Nenhuma porta detectada", id="__none__", disabled=True))
            return
        for port in ports:
            widget.add_option(Option(port.label(), id=port.device))
        widget.highlighted = 0

    def _selected_mode(self) -> str:
        widget = self.query_one("#connection-mode-list", OptionList)
        option = widget.get_option_at_index(widget.highlighted or 0)
        return option.id or "auto"

    def _selected_port(self) -> str:
        manual = self.query_one("#manual-port", Input).value.strip()
        if manual:
            return manual
        widget = self.query_one("#port-list", OptionList)
        if widget.option_count == 0:
            return ""
        option = widget.get_option_at_index(widget.highlighted or 0)
        return "" if option.id == "__none__" else str(option.id or "")

    def action_refresh_ports(self) -> None:
        self._populate_ports()

    def action_cancel(self) -> None:
        self.dismiss(None)

    def action_demo(self) -> None:
        self.dismiss(ConnectionChoice(demo=True, mode="gateway"))

    @on(Button.Pressed, "#connect-button")
    def _connect(self) -> None:
        port = self._selected_port()
        if not port:
            self.notify("Informe uma porta serial", severity="warning")
            return
        self.dismiss(ConnectionChoice(port=port, mode=self._selected_mode()))

    @on(Button.Pressed, "#demo-button")
    def _demo(self) -> None:
        self.action_demo()

    @on(Button.Pressed, "#cancel-button")
    def _cancel(self) -> None:
        self.action_cancel()

    @on(Input.Submitted, "#manual-port")
    def _manual_submit(self) -> None:
        self._connect()


class HelpScreen(ModalScreen[None]):
    BINDINGS = [
        Binding("escape", "close", "Fechar", show=True),
        Binding("f1", "close", "Fechar", show=False),
        Binding("enter", "close", "Fechar", show=False),
    ]

    HELP_TEXT = """\
[b]Navegação principal[/b]
  F1 Ajuda             F2 Menu             F3 Selecionar nó/sensor
  F4 Configuração      F5 Solicitar status F6 Telemetria
  F7 FFT               F8 DTC              F9 Rede CAN FD
  F10 Sair             F11 Compacto        F12 Snapshot

[b]Atalhos operacionais[/b]
  Ctrl+T Telemetria ON/OFF      Ctrl+F FFT do sensor selecionado
  Ctrl+D DTC                    Ctrl+R Reconectar
  Ctrl+P Pausar atualização     Ctrl+G Gateway
  Ctrl+N Focar árvore de nós          Ctrl+E Eventos
  Ctrl+K Limpar DTC             Ctrl+M Simulação ON/OFF
  Ctrl+O Reiniciar POLLING      Ctrl+W Wi-Fi ON
  Ctrl+Y Estado segurança       Ctrl+C Parar telemetria (0x03)

[b]Comandos internos[/b]
  :status                       :node 20.01
  :tel on 20.01                 :tel rate normal
  :fft once 20.01 bins=64       :dtc list 20.01
  :dtc clear 20.01 all          :acq polling
  :config 20.01 rate=250 window=hann
  :tel once / fast / slow       :tel period 1000
  :wifi on / off / status       :security / :lock / :unlock <otp>
  :export csv                   :disconnect / :reconnect / :quit

[b]Comandos diretos da baseline[/b]
  STATUS, GET, SET ..., APPLY, TELEMETRY ..., FFT ONCE
  SIMULATE ON/OFF, ACQ POLLING, DTC, DTC CLEAR, NET, NET WIFI ..., VERSION, PING, RESET
  ACQ DRDY e comandos SLEEP/WAKE não fazem parte da baseline atual.

No modo SENSOR_DIRECT, comandos sem ':' são enviados diretamente ao console
ASCII do firmware. No modo GATEWAY_CAN, comandos sem ':' são enviados como
linhas brutas ao gateway para diagnóstico.

[dim]Esc, Enter ou F1 fecha esta janela.[/dim]
"""

    def compose(self) -> ComposeResult:
        with VerticalScroll(id="help-dialog", classes="modal"):
            yield Static("AJUDA", classes="modal-title")
            yield Static(self.HELP_TEXT, id="help-body")

    def action_close(self) -> None:
        self.dismiss(None)


class MainMenuScreen(ModalScreen[str | None]):
    BINDINGS = [Binding("escape", "cancel", "Fechar", show=False)]
    ITEMS = [
        ("Conexão", "connection"),
        ("Nós", "nodes"),
        ("Telemetria", "telemetry"),
        ("FFT", "fft"),
        ("Diagnóstico", "dtc"),
        ("Configuração", "config"),
        ("Rede CAN FD", "network"),
        ("Wi-Fi sensor ON", "wifi_on"),
        ("Wi-Fi sensor OFF", "wifi_off"),
        ("Gateway", "gateway"),
        ("Logs e exportação", "logs"),
        ("Ajuda", "help"),
        ("Sair", "quit"),
    ]

    def compose(self) -> ComposeResult:
        with Vertical(id="menu-dialog", classes="modal"):
            yield Static("MENU PRINCIPAL", classes="modal-title")
            yield OptionList(*[Option(label, id=action) for label, action in self.ITEMS], id="main-menu-list")

    def on_mount(self) -> None:
        widget = self.query_one("#main-menu-list", OptionList)
        widget.highlighted = 0
        widget.focus()

    @on(OptionList.OptionSelected, "#main-menu-list")
    def _selected(self, event: OptionList.OptionSelected) -> None:
        self.dismiss(str(event.option.id))

    def action_cancel(self) -> None:
        self.dismiss(None)


class NodeNavigatorScreen(ModalScreen[str | None]):
    """Janela F3 para navegar e selecionar sensores com as setas."""

    BINDINGS = [
        Binding("escape", "cancel", "Cancelar", show=False),
    ]

    def __init__(self, state: AppState, current_logical_id: str | None = None) -> None:
        super().__init__()
        self.state = state
        self.current_logical_id = current_logical_id

    def _options(self) -> list[Option]:
        options: list[Option] = []
        if not self.state.nodes:
            return [Option("Nenhum nó disponível", id="__empty__", disabled=True)]
        for parent_id, node in sorted(self.state.nodes.items()):
            node_marker = palette.STATUS_MARKERS.get(node.status.value, "[N/A]")
            options.append(
                Option(
                    f"{node_marker} Node {parent_id:02d} — {node.node_type}",
                    id=f"node:{parent_id}",
                    disabled=True,
                )
            )
            for child_id, sensor in sorted(node.sensors.items()):
                quality = palette.QUALITY_MARKERS.get(sensor.quality.value, "[N/A]")
                uuid = f"  UUID={sensor.wireless_uuid}" if sensor.wireless_uuid else ""
                severity = sensor.highest_severity.value if sensor.highest_severity else "OK"
                options.append(
                    Option(
                        f"   ↳ {quality} {sensor.logical_id}  {sensor.status.value}  "
                        f"{sensor.sensor_mode.value}  DTC={severity}{uuid}",
                        id=sensor.logical_id,
                    )
                )
        if not any(not option.disabled for option in options):
            options.append(Option("Nenhum sensor associado", id="__empty__", disabled=True))
        return options

    def compose(self) -> ComposeResult:
        with Vertical(id="node-navigator-dialog", classes="modal wide-modal"):
            yield Static("SELECIONAR NÓ / SENSOR", classes="modal-title")
            yield Static(
                "Use ↑/↓ para navegar e Enter para selecionar. Os cabeçalhos dos nós físicos "
                "organizam os sensores associados.",
                classes="modal-hint",
            )
            yield OptionList(*self._options(), id="node-navigator-list")
            with Horizontal(classes="modal-buttons"):
                yield Button("Selecionar", id="node-nav-select", variant="primary")
                yield Button("Cancelar", id="node-nav-cancel")

    def on_mount(self) -> None:
        widget = self.query_one("#node-navigator-list", OptionList)
        target_index: int | None = None
        first_enabled: int | None = None
        for index in range(widget.option_count):
            option = widget.get_option_at_index(index)
            if not option.disabled and first_enabled is None:
                first_enabled = index
            if option.id == self.current_logical_id:
                target_index = index
        widget.highlighted = target_index if target_index is not None else first_enabled
        widget.focus()

    def _selected_id(self) -> str | None:
        widget = self.query_one("#node-navigator-list", OptionList)
        if widget.highlighted is None:
            return None
        option = widget.get_option_at_index(widget.highlighted)
        if option.disabled or option.id in {None, "__empty__"}:
            return None
        return str(option.id)

    @on(OptionList.OptionSelected, "#node-navigator-list")
    def _option_selected(self, event: OptionList.OptionSelected) -> None:
        if event.option.id and not event.option.disabled:
            self.dismiss(str(event.option.id))

    @on(Button.Pressed, "#node-nav-select")
    def _select_button(self) -> None:
        logical_id = self._selected_id()
        if logical_id:
            self.dismiss(logical_id)
        else:
            self.notify("Selecione um sensor", severity="warning")

    @on(Button.Pressed, "#node-nav-cancel")
    def _cancel_button(self) -> None:
        self.dismiss(None)

    def action_cancel(self) -> None:
        self.dismiss(None)


class ConfigScreen(ModalScreen[ConfigRequest | None]):
    BINDINGS = [Binding("escape", "cancel", "Cancelar", show=False)]

    def __init__(self, sensor: SensorNode) -> None:
        super().__init__()
        self.sensor = sensor

    def compose(self) -> ComposeResult:
        cfg = self.sensor.configuration
        with VerticalScroll(id="config-dialog", classes="modal wide-modal"):
            yield Static(f"CONFIGURAR {self.sensor.logical_id}", classes="modal-title")
            yield Label("Modo")
            yield OptionList(*[Option(v, id=v) for v in commands.DEFAULT_FSM_MODES], id="cfg-mode")
            yield Label("Janela")
            yield OptionList(*[Option(v, id=v) for v in commands.DEFAULT_WINDOW_TYPES], id="cfg-window")
            yield Label("Taxa de amostragem [Hz]")
            yield Input(value=_number(cfg.sample_rate_requested_hz), id="cfg-rate")
            yield Label("Tamanho da janela")
            yield Input(value=str(cfg.window_size or 512), id="cfg-size")
            yield Label("Limiar STA/LTA")
            yield Input(value=_number(cfg.stalta_threshold), id="cfg-stalta")
            yield Label("Ganho/calibração")
            yield Input(value=_number(cfg.calibration_gain), id="cfg-gain")
            with Horizontal(classes="modal-buttons"):
                yield Button("Aplicar", id="cfg-apply")
                yield Button("Aplicar e verificar", id="cfg-verify", variant="primary")
                yield Button("Cancelar", id="cfg-cancel")

    def on_mount(self) -> None:
        self._select_option("#cfg-mode", self.sensor.configuration.mode.value)
        self._select_option("#cfg-window", self.sensor.configuration.window_type or "HANN")

    def _select_option(self, selector: str, value: str) -> None:
        widget = self.query_one(selector, OptionList)
        for index in range(widget.option_count):
            if widget.get_option_at_index(index).id == value:
                widget.highlighted = index
                return
        widget.highlighted = 0

    def _build(self, verify: bool) -> ConfigRequest | None:
        try:
            mode_list = self.query_one("#cfg-mode", OptionList)
            win_list = self.query_one("#cfg-window", OptionList)
            mode = str(mode_list.get_option_at_index(mode_list.highlighted or 0).id)
            window = str(win_list.get_option_at_index(win_list.highlighted or 0).id)
            return ConfigRequest(
                mode=mode,
                window=window,
                rate_hz=_float_or_none(self.query_one("#cfg-rate", Input).value),
                window_size=_int_or_none(self.query_one("#cfg-size", Input).value),
                stalta=_float_or_none(self.query_one("#cfg-stalta", Input).value),
                gain=_float_or_none(self.query_one("#cfg-gain", Input).value),
                apply_and_verify=verify,
            )
        except ValueError as exc:
            self.notify(str(exc), severity="error")
            return None

    @on(Button.Pressed, "#cfg-apply")
    def _apply(self) -> None:
        if request := self._build(False):
            self.dismiss(request)

    @on(Button.Pressed, "#cfg-verify")
    def _verify(self) -> None:
        if request := self._build(True):
            self.dismiss(request)

    @on(Button.Pressed, "#cfg-cancel")
    def _cancel_button(self) -> None:
        self.dismiss(None)

    def action_cancel(self) -> None:
        self.dismiss(None)


class FftRequestScreen(ModalScreen[FftRequest | None]):
    BINDINGS = [Binding("escape", "cancel", "Cancelar", show=False)]

    def __init__(self, logical_id: str) -> None:
        super().__init__()
        self.logical_id = logical_id

    def compose(self) -> ComposeResult:
        with Vertical(id="fft-request-dialog", classes="modal"):
            yield Static(f"SOLICITAR FFT — {self.logical_id}", classes="modal-title")
            yield Label("Quantidade de bins")
            yield OptionList(*[Option(str(v), id=str(v)) for v in commands.FFT_BIN_OPTIONS], id="fft-bins")
            yield Label("Modo")
            yield OptionList(
                Option("Visualizar uma vez", id="VIEW_ONLY"),
                Option("Salvar em arquivo", id="SAVE_TO_FILE"),
                Option("Registrar janela", id="LOG_WINDOW"),
                id="fft-mode",
            )
            yield Static("64 bins é a opção padrão. 128/256 aumentam a carga do barramento.", classes="modal-hint")
            with Horizontal(classes="modal-buttons"):
                yield Button("Solicitar", id="fft-confirm", variant="primary")
                yield Button("Cancelar", id="fft-cancel")

    def on_mount(self) -> None:
        self.query_one("#fft-bins", OptionList).highlighted = 1
        self.query_one("#fft-mode", OptionList).highlighted = 0

    @on(Button.Pressed, "#fft-confirm")
    def _confirm(self) -> None:
        bins_list = self.query_one("#fft-bins", OptionList)
        mode_list = self.query_one("#fft-mode", OptionList)
        bins = int(str(bins_list.get_option_at_index(bins_list.highlighted or 1).id))
        mode = str(mode_list.get_option_at_index(mode_list.highlighted or 0).id)
        self.dismiss(FftRequest(bins=bins, mode=mode))

    @on(Button.Pressed, "#fft-cancel")
    def _cancel_button(self) -> None:
        self.dismiss(None)

    def action_cancel(self) -> None:
        self.dismiss(None)

class TelemetryScreen(ModalScreen[None]):
    """Telemetria detalhada e continuamente atualizada do sensor selecionado.

    Abrir ou fechar a tela não altera o streaming do firmware. Os comandos de
    aquisição somente são enviados por ações explícitas do operador.
    """

    BINDINGS = [
        Binding("escape", "close", "Fechar", show=True),
        Binding("space", "toggle_visual_pause", "Pausar gráfico", show=True),
        Binding("o", "once", "Amostra única", show=False),
    ]

    ROWS: tuple[tuple[str, str], ...] = (
        ("mode", "Modo FSM"),
        ("acquisition", "Aquisição"),
        ("axis", "Eixo"),
        ("rate", "Taxa de amostragem"),
        ("window", "Janela / tamanho"),
        ("rms", "RMS"),
        ("kurtosis", "Curtose (excesso)"),
        ("crest", "Fator de crista"),
        ("peak_hz", "Frequência dominante"),
        ("peak_amp", "Amplitude dominante"),
        ("entropy", "Entropia espectral"),
        ("ppv", "PPV"),
        ("stalta", "STA/LTA"),
        ("clip", "Clipping"),
        ("fft_valid", "FFT resumida"),
        ("battery", "Bateria"),
        ("dtc", "DTCs ativos"),
        ("quality", "Qualidade"),
        ("sequence", "Sequência / perda"),
        ("updated", "Última atualização"),
    )

    def __init__(
        self,
        logical_id: str,
        sensor_provider: Callable[[str], SensorNode | None],
    ) -> None:
        super().__init__()
        self.logical_id = logical_id
        self._sensor_provider = sensor_provider
        self._visual_paused = False

    def compose(self) -> ComposeResult:
        sensor = self._sensor_provider(self.logical_id)
        period = sensor.health.telemetry_period_ms if sensor else None
        with Vertical(id="telemetry-dialog", classes="modal telemetry-modal"):
            yield Static(f"TELEMETRIA DETALHADA — {self.logical_id}", classes="modal-title")
            yield Static("Aguardando dados...", id="telemetry-live-status")
            with Horizontal(id="telemetry-live-body"):
                yield DataTable(id="telemetry-live-table", cursor_type="row", zebra_stripes=False)
                with Vertical(id="telemetry-trends"):
                    yield Label("TENDÊNCIA RMS", classes="section-title")
                    yield Sparkline([], id="telemetry-rms-sparkline")
                    yield Label("TENDÊNCIA PPV", classes="section-title")
                    yield Sparkline([], id="telemetry-ppv-sparkline")
                    yield Static("Espaço: pausar apenas a visualização", classes="modal-hint")
            with Horizontal(id="telemetry-controls"):
                yield Button("Iniciar", id="tel-start", variant="primary")
                yield Button("Parar", id="tel-stop")
                yield Button("Amostra única", id="tel-once")
                yield Button("Rápido", id="tel-fast")
                yield Button("Lento", id="tel-slow")
            with Horizontal(id="telemetry-period-controls"):
                yield Label("Período [ms]")
                yield Input(value=str(period or 1000), id="tel-period-input")
                yield Button("Aplicar período", id="tel-period-apply")
                yield Button("Fechar", id="tel-close")

    def on_mount(self) -> None:
        table = self.query_one("#telemetry-live-table", DataTable)
        table.add_column("Métrica", key="metric", width=27)
        table.add_column("Valor", key="value", width=28)
        for key, label in self.ROWS:
            table.add_row(label, "N/A", key=key)
        self.set_interval(0.25, self._refresh_live)
        self._refresh_live()

    def _refresh_live(self) -> None:
        if self._visual_paused:
            return
        sensor = self._sensor_provider(self.logical_id)
        if sensor is None:
            self.query_one("#telemetry-live-status", Static).update("Sensor não encontrado")
            return
        sample = sensor.latest_telemetry
        stream = "LIGADA" if sensor.health.telemetry_enabled else "DESLIGADA"
        acq_ok = "OK" if sensor.acquisition_mode.value in {"POLLING", "SIM", "IDLE"} else "EXPERIMENTAL"
        drdy = "desativado" if sensor.acquisition_mode.value == "POLLING" else "experimental/legado"
        self.query_one("#telemetry-live-status", Static).update(
            f"Estado: [b]{sensor.status.value}[/b]  •  Stream: [b]{stream}[/b]  •  "
            f"Aquisição: [b]{sensor.acquisition_mode.value}[/b] ({acq_ok})  •  DRDY: {drdy}"
        )
        if sample is None:
            self._update_values(
                {
                    "mode": sensor.sensor_mode.value,
                    "acquisition": sensor.acquisition_mode.value,
                    "dtc": str(sensor.active_dtc_count),
                }
            )
            return

        age = max(0.0, time.time() - sample.received_wall_time)
        battery = "N/A"
        if sample.battery.valid:
            parts: list[str] = []
            if sample.battery.percentage is not None:
                parts.append(f"{sample.battery.percentage:.0f}%")
            if sample.battery.voltage_v is not None:
                parts.append(f"{sample.battery.voltage_v:.3f} V")
            battery = " / ".join(parts) or "N/A"
        rate = sample.sample_rate_effective_hz or sample.sample_rate_requested_hz
        values = {
            "mode": sample.mode.value,
            "acquisition": sample.acquisition_mode.value,
            "axis": sample.axis or "N/A",
            "rate": _format_value(rate, 2, "Hz"),
            "window": f"{sample.window_type or 'N/A'} / {sample.window_size or 'N/A'}",
            "rms": _format_value(sample.rms, 5, sample.rms_unit),
            "kurtosis": _format_value(sample.kurtosis, 5),
            "crest": _format_value(sample.crest_factor, 4),
            "peak_hz": _fft_value(sample.peak_frequency_hz, sample.fft_valid, 3, "Hz"),
            "peak_amp": _fft_value(sample.peak_amplitude, sample.fft_valid, 6),
            "entropy": _fft_value(sample.spectral_entropy, sample.fft_valid, 4),
            "ppv": _format_value(sample.ppv_mm_s, 4, "mm/s"),
            "stalta": _bool_value(sample.stalta_triggered),
            "clip": "SATURADO" if sample.clipping is True else ("NÃO" if sample.clipping is False else "N/A"),
            "fft_valid": _fft_valid_value(sample.fft_valid),
            "battery": battery,
            "dtc": str(sample.dtc_count if sample.dtc_count is not None else sensor.active_dtc_count),
            "quality": f"{palette.QUALITY_MARKERS.get(sample.quality.value, '[N/A]')} {sample.quality.value}",
            "sequence": f"{sample.sequence if sample.sequence is not None else 'N/A'} / {sensor.loss_percent:.2f}%",
            "updated": f"há {age:.1f} s",
        }
        self._update_values(values)
        history = list(sensor.telemetry_history)[-120:]
        self.query_one("#telemetry-rms-sparkline", Sparkline).data = [
            item.rms for item in history if item.rms is not None
        ]
        self.query_one("#telemetry-ppv-sparkline", Sparkline).data = [
            item.ppv_mm_s for item in history if item.ppv_mm_s is not None
        ]

    def _update_values(self, values: dict[str, object]) -> None:
        table = self.query_one("#telemetry-live-table", DataTable)
        for key, _ in self.ROWS:
            table.update_cell(key, "value", str(values.get(key, "N/A")))

    def _request(self, action: str, value: int | None = None) -> None:
        self.post_message(TelemetryCommandRequested(self.logical_id, action, value))

    def action_close(self) -> None:
        self.dismiss(None)

    def action_toggle_visual_pause(self) -> None:
        self._visual_paused = not self._visual_paused
        label = (
            "VISUALIZAÇÃO PAUSADA — a recepção e o log continuam"
            if self._visual_paused
            else "Visualização retomada"
        )
        self.query_one("#telemetry-live-status", Static).update(label)
        if not self._visual_paused:
            self._refresh_live()

    def action_once(self) -> None:
        self._request("ONCE")

    @on(Button.Pressed, "#tel-start")
    def _start(self) -> None:
        self._request("ON")

    @on(Button.Pressed, "#tel-stop")
    def _stop(self) -> None:
        self._request("OFF")

    @on(Button.Pressed, "#tel-once")
    def _once(self) -> None:
        self._request("ONCE")

    @on(Button.Pressed, "#tel-fast")
    def _fast(self) -> None:
        self._request("FAST")

    @on(Button.Pressed, "#tel-slow")
    def _slow(self) -> None:
        self._request("SLOW")

    @on(Button.Pressed, "#tel-period-apply")
    def _period(self) -> None:
        try:
            value = int(self.query_one("#tel-period-input", Input).value.strip())
            if value <= 0:
                raise ValueError
        except ValueError:
            self.notify("Período inválido", severity="error")
            return
        self._request("PERIOD", value)

    @on(Button.Pressed, "#tel-close")
    def _close_button(self) -> None:
        self.action_close()


class FftViewScreen(ModalScreen[None]):
    BINDINGS = [Binding("escape", "close", "Fechar", show=False), Binding("enter", "close", "Fechar", show=False)]

    def __init__(
        self,
        logical_id: str,
        sensor_provider: Callable[[str], SensorNode | None],
        *,
        requested_at: float | None = None,
    ) -> None:
        super().__init__()
        self.logical_id = logical_id
        self._sensor_provider = sensor_provider
        self.requested_at = requested_at or 0.0

    def compose(self) -> ComposeResult:
        with VerticalScroll(id="fft-view-dialog", classes="modal extra-wide-modal"):
            yield Static(f"ESPECTRO FFT — {self.logical_id}", classes="modal-title")
            yield Static("Aguardando resposta FFT...", id="fft-summary")
            yield Static("", id="fft-ascii")
            yield Static("", id="fft-peaks")
            yield Static("Esc ou Enter: fechar", classes="modal-hint")

    def on_mount(self) -> None:
        self.set_interval(0.25, self._refresh_spectrum)
        self._refresh_spectrum()

    def _refresh_spectrum(self) -> None:
        sensor = self._sensor_provider(self.logical_id)
        summary = self.query_one("#fft-summary", Static)
        chart = self.query_one("#fft-ascii", Static)
        peaks = self.query_one("#fft-peaks", Static)
        if sensor is None:
            summary.update("Sensor não encontrado.")
            chart.update("")
            peaks.update("")
            return
        telemetry = sensor.latest_telemetry
        if telemetry is not None and telemetry.fft_valid is False:
            summary.update("FFT não calculada para o buffer atual. Comportamento esperado no modo SEISMIC.")
            chart.update("")
            peaks.update("")
            return
        spectrum = sensor.latest_fft
        if spectrum is None or spectrum.received_wall_time < self.requested_at:
            summary.update("Aguardando linha FFT válida e completa...")
            chart.update("O vetor FFT não faz parte de TEL; ele é recebido separadamente após FFT ONCE.")
            peaks.update("")
            return
        summary.update(_spectrum_metadata(spectrum))
        chart.update(_spectrum_chart(spectrum))
        peaks.update(_spectrum_peaks(spectrum))

    def action_close(self) -> None:
        self.dismiss(None)

class DtcScreen(ModalScreen[str | None]):
    BINDINGS = [
        Binding("escape", "close", "Fechar", show=False),
        Binding("c", "clear_selected", "Limpar selecionado", show=True),
        Binding("a", "clear_all", "Limpar todos", show=True),
    ]

    def __init__(self, sensor: SensorNode) -> None:
        super().__init__()
        self.sensor = sensor

    def compose(self) -> ComposeResult:
        with Vertical(id="dtc-dialog", classes="modal extra-wide-modal"):
            yield Static(f"DIAGNÓSTICOS — {self.sensor.logical_id}", classes="modal-title")
            yield DataTable(id="dtc-table", cursor_type="row")
            with Horizontal(classes="modal-buttons"):
                yield Button("Limpar selecionado", id="dtc-clear-one")
                yield Button("Limpar todos", id="dtc-clear-all")
                yield Button("Fechar", id="dtc-close", variant="primary")

    def on_mount(self) -> None:
        table = self.query_one("#dtc-table", DataTable)
        table.add_columns("Código", "Descrição", "Severidade", "Sintoma", "Ocorrências", "Estado")
        for record in sorted(self.sensor.active_dtcs.values(), key=lambda item: item.code):
            table.add_row(
                f"0x{record.code:04X}",
                dtc_description(record.code),
                record.severity.value,
                f"0x{record.symptom:02X}" if record.symptom is not None else "N/A",
                str(record.occurrence_count),
                "ATIVO" if record.active else "HISTÓRICO",
                key=str(record.code),
            )

    def action_close(self) -> None:
        self.dismiss(None)

    def action_clear_selected(self) -> None:
        table = self.query_one("#dtc-table", DataTable)
        if table.row_count and table.cursor_row >= 0:
            key = table.get_row_at(table.cursor_row)[0]
            self.dismiss(f"one:{key}")

    def action_clear_all(self) -> None:
        self.dismiss("all")

    @on(Button.Pressed, "#dtc-clear-one")
    def _one(self) -> None:
        self.action_clear_selected()

    @on(Button.Pressed, "#dtc-clear-all")
    def _all(self) -> None:
        self.action_clear_all()

    @on(Button.Pressed, "#dtc-close")
    def _close_button(self) -> None:
        self.action_close()


class NetworkScreen(ModalScreen[None]):
    BINDINGS = [Binding("escape", "close", "Fechar", show=False), Binding("enter", "close", "Fechar", show=False)]

    def __init__(self, state: AppState) -> None:
        super().__init__()
        self.state = state

    def compose(self) -> ComposeResult:
        gw = self.state.gateway
        net = self.state.network
        with VerticalScroll(id="network-dialog", classes="modal extra-wide-modal"):
            yield Static("REDE CAN FD", classes="modal-title")
            yield Static(
                f"Arbitragem: {gw.arbitration_bitrate or 'N/A'} bit/s\n"
                f"Dados: {gw.data_bitrate or 'N/A'} bit/s\n"
                f"CAN: {gw.can_state}  Wi-Fi: {gw.wifi_state}\n"
                f"Frames RX/TX: {net.frames_rx}/{net.frames_tx}\n"
                f"Bytes RX/TX: {net.bytes_rx}/{net.bytes_tx}\n"
                f"CRC: {net.crc_errors}  Parse: {net.parse_errors}\n"
                f"Bus-off: {net.bus_off}  Error passive: {net.error_passive}\n"
                f"Utilização: {net.utilization_percent if net.utilization_percent is not None else 'N/A'}%\n"
                f"Transferências ativas: {net.active_transfers}"
            )
            yield Static("FRAMES RECENTES", classes="section-title")
            yield Static(_frames_text(list(net.recent_frames)[-30:]), id="can-frame-list")

    def action_close(self) -> None:
        self.dismiss(None)


class NodeDetailScreen(ModalScreen[None]):
    BINDINGS = [Binding("escape", "close", "Fechar", show=False), Binding("enter", "close", "Fechar", show=False)]

    def __init__(self, sensor: SensorNode) -> None:
        super().__init__()
        self.sensor = sensor

    def compose(self) -> ComposeResult:
        cfg = self.sensor.configuration
        health = self.sensor.health
        if self.sensor.acquisition_mode.value == "POLLING":
            drdy_text = "DRDY: desativado (POLLING é a baseline saudável)\n"
        elif self.sensor.acquisition_mode.value == "DRDY":
            drdy_text = (
                f"DRDY IRQ: {health.drdy_irq_count if health.drdy_irq_count is not None else 'N/A'}\n"
                f"DRDY perdidas: {health.drdy_missed_count if health.drdy_missed_count is not None else 'N/A'}\n"
            )
        else:
            drdy_text = "DRDY: N/A\n"
        with VerticalScroll(id="node-detail-dialog", classes="modal"):
            yield Static(f"DETALHE — {self.sensor.logical_id}", classes="modal-title")
            yield Static(
                f"UUID: {self.sensor.wireless_uuid or 'N/A'}\n"
                f"Estado: {self.sensor.status.value}\n"
                f"Qualidade: {self.sensor.quality.value}\n"
                f"Modo: {self.sensor.sensor_mode.value}\n"
                f"Aquisição: {self.sensor.acquisition_mode.value}\n"
                f"Taxa solicitada/efetiva: {cfg.sample_rate_requested_hz or 'N/A'} / {cfg.sample_rate_effective_hz or 'N/A'} Hz\n"
                f"Janela: {cfg.window_type or 'N/A'} ({cfg.window_size or 'N/A'})\n"
                f"{drdy_text}"
                f"RX: {self.sensor.rx_count}  Perdidas: {self.sensor.lost_count}\n"
                f"Duplicadas: {self.sensor.duplicate_count}  Fora de ordem: {self.sensor.out_of_order_count}\n"
                f"DTCs ativos: {self.sensor.active_dtc_count}"
            )

    def action_close(self) -> None:
        self.dismiss(None)


class ConfirmScreen(ModalScreen[bool]):
    BINDINGS = [Binding("escape", "no", "Não", show=False), Binding("n", "no", "Não", show=False), Binding("y", "yes", "Sim", show=False)]

    def __init__(self, title: str, message: str) -> None:
        super().__init__()
        self.dialog_title = title
        self.message = message

    def compose(self) -> ComposeResult:
        with Vertical(id="confirm-dialog", classes="modal"):
            yield Static(self.dialog_title, classes="modal-title")
            yield Static(self.message)
            with Horizontal(classes="modal-buttons"):
                yield Button("Não", id="confirm-no")
                yield Button("Sim", id="confirm-yes", variant="error")

    def action_yes(self) -> None:
        self.dismiss(True)

    def action_no(self) -> None:
        self.dismiss(False)

    @on(Button.Pressed, "#confirm-yes")
    def _yes(self) -> None:
        self.action_yes()

    @on(Button.Pressed, "#confirm-no")
    def _no(self) -> None:
        self.action_no()


def _number(value: float | None) -> str:
    return "" if value is None else f"{value:g}"


def _float_or_none(text: str) -> float | None:
    text = text.strip().replace(",", ".")
    if not text:
        return None
    value = float(text)
    if value != value or value in {float("inf"), float("-inf")}:
        raise ValueError("Valor numérico inválido")
    return value


def _int_or_none(text: str) -> int | None:
    text = text.strip()
    if not text:
        return None
    value = int(text, 0)
    if value <= 0:
        raise ValueError("O tamanho deve ser positivo")
    return value


def _format_value(value: object, decimals: int = 3, unit: str = "") -> str:
    if value is None:
        return "N/A"
    try:
        text = f"{float(value):.{decimals}f}"
    except (TypeError, ValueError):
        text = str(value)
    return f"{text} {unit}".strip()


def _bool_value(value: bool | None) -> str:
    if value is None:
        return "N/A"
    return "SIM" if value else "NÃO"


def _fft_valid_value(value: bool | None) -> str:
    if value is True:
        return "VÁLIDA"
    if value is False:
        return "NÃO CALCULADA (esperado no modo atual)"
    return "N/A"


def _fft_value(value: object, fft_valid: bool | None, decimals: int, unit: str = "") -> str:
    if fft_valid is False:
        return "N/A (FFT desativada)"
    return _format_value(value, decimals, unit)


def _spectrum_frequency(spectrum: SpectrumSample, bin_index: int) -> float | None:
    if spectrum.sample_rate_hz is None or not spectrum.fft_size:
        return None
    return bin_index * spectrum.sample_rate_hz / spectrum.fft_size


def _spectrum_metadata(spectrum: SpectrumSample) -> str:
    bins = len(spectrum.magnitudes)
    resolution = None
    if spectrum.sample_rate_hz is not None and spectrum.fft_size:
        resolution = spectrum.sample_rate_hz / spectrum.fft_size
    max_frequency = _spectrum_frequency(spectrum, max(0, bins - 1))
    nyquist = spectrum.sample_rate_hz / 2 if spectrum.sample_rate_hz is not None else None
    return (
        f"Bins recebidos: {bins}  •  FFT: {spectrum.fft_size or 'N/A'} pontos  •  "
        f"Fs: {_format_value(spectrum.sample_rate_hz, 2, 'Hz')}  •  "
        f"Δf: {_format_value(resolution, 4, 'Hz')}\n"
        f"Janela: {spectrum.window_type or 'N/A'}  •  Unidade vertical: {spectrum.magnitude_unit or 'raw'}  •  "
        f"Faixa exibida: 0 a {_format_value(max_frequency, 2, 'Hz')}  •  "
        f"Nyquist: {_format_value(nyquist, 2, 'Hz')}"
    )


def _spectrum_chart(spectrum: SpectrumSample, width: int = 72, height: int = 14) -> str:
    values = spectrum.magnitudes
    if not values:
        return "N/A"
    chart_width = max(16, min(width, len(values)))
    buckets: list[tuple[float, int]] = []
    for column in range(chart_width):
        start = column * len(values) // chart_width
        end = max(start + 1, (column + 1) * len(values) // chart_width)
        local = values[start:end]
        local_index = max(range(len(local)), key=lambda idx: local[idx])
        buckets.append((float(local[local_index]), start + local_index))
    peak_value = max(value for value, _ in buckets)
    scale = peak_value if peak_value > 0 else 1.0
    lines: list[str] = []
    for row in range(height, 0, -1):
        threshold = scale * row / height
        label = f"{threshold:>9.3g} ┤"
        body = "".join("█" if value >= threshold else " " for value, _ in buckets)
        lines.append(label + body)
    lines.append(f"{0:>9.3g} └" + "─" * chart_width)

    max_frequency = _spectrum_frequency(spectrum, len(values) - 1)
    if max_frequency is None:
        left, middle, right = "bin 0", f"bin {(len(values) - 1) // 2}", f"bin {len(values) - 1}"
        axis_name = "Bins espectrais"
    else:
        left, middle, right = "0 Hz", f"{max_frequency / 2:.2f} Hz", f"{max_frequency:.2f} Hz"
        axis_name = "Frequência"
    gap1 = max(1, chart_width // 2 - len(left) - len(middle) // 2)
    gap2 = max(1, chart_width - len(left) - gap1 - len(middle) - len(right))
    lines.append(" " * 11 + left + " " * gap1 + middle + " " * gap2 + right)
    lines.append(f"Magnitude [{spectrum.magnitude_unit or 'raw'}] × {axis_name}")
    return "\n".join(lines)


def _spectrum_peaks(spectrum: SpectrumSample, count: int = 5) -> str:
    if not spectrum.magnitudes:
        return ""
    ranked = sorted(enumerate(spectrum.magnitudes), key=lambda item: item[1], reverse=True)[:count]
    rows = ["PICOS PRINCIPAIS"]
    for position, (index, value) in enumerate(ranked, start=1):
        frequency = _spectrum_frequency(spectrum, index)
        frequency_text = f"{frequency:.3f} Hz" if frequency is not None else f"bin {index}"
        rows.append(f"{position}. {frequency_text:<16} magnitude={value:.6g}  bin={index}")
    return "\n".join(rows)

def _frames_text(frames: list[Any]) -> str:
    if not frames:
        return "Nenhum frame recebido."
    return "\n".join(
        f"{frame.direction} 0x{frame.can_id:08X} {'FD' if frame.fd else 'CAN'} "
        f"DLC={len(frame.data):02d} {frame.data.hex(' ')}"
        for frame in frames
    )
