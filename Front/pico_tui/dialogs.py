"""Diálogos modais da TUI.

Mantêm decisões curtas e locais fora das telas de contexto: conexão serial,
confirmação de ações, parâmetros, configuração, escolha de alvo, escolha do
módulo responsável por uma associação wireless e escolhas simples em lista.
"""
from __future__ import annotations

from dataclasses import dataclass

from rich.text import Text
from textual import on
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, VerticalScroll
from textual.screen import ModalScreen
from textual.widgets import Button, Input, Label, OptionList, Select, Static
from textual.widgets.option_list import Option

from pico_tui import commands, palette
from pico_tui.command_catalog import CommandAction
from pico_tui.core.models import SensorNode
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


class ConnectScreen(ModalScreen[ConnectionChoice | None]):
    """Escolha da porta serial e do protocolo."""

    BINDINGS = [
        Binding("escape", "cancel", "Cancelar", show=False),
        Binding("f5", "refresh_ports", "Atualizar portas", show=False),
        Binding("ctrl+r", "refresh_ports", "Atualizar portas", show=False),
        Binding("ctrl+d", "demo", "Demonstração", show=False),
    ]

    def __init__(self, initial_port: str = "", initial_mode: str = "auto", *, allow_cancel: bool = True) -> None:
        super().__init__()
        self.initial_port = initial_port or ""
        self.initial_mode = (initial_mode or "auto").lower()
        self.allow_cancel = allow_cancel

    def compose(self) -> ComposeResult:
        with VerticalScroll(id="connect-dialog", classes="dialog"):
            yield Static("Conectar", classes="dialog-title")
            yield Static(
                "Probe 00 (ESP32): /dev/ttyUSB* ou COMx. Sensor Pico W: /dev/ttyACM*. "
                "Ctrl+R atualiza a lista.",
                classes="dialog-hint",
            )
            yield Label("Portas detectadas")
            yield OptionList(id="port-list", markup=False)
            yield Label("Ou informe o caminho")
            yield Input(placeholder="/dev/ttyUSB0, COM5 ou socket://host:porta", id="manual-port")
            yield Label("Equipamento conectado")
            yield Select(
                [
                    ("Detectar automaticamente", "auto"),
                    ("Probe 00 na rede CAN", "gateway"),
                    ("Sensor Pico W por USB", "sensor"),
                ],
                value=self.initial_mode if self.initial_mode in {"auto", "gateway", "sensor"} else "auto",
                allow_blank=False,
                id="connection-mode",
            )
            with Horizontal(classes="dialog-buttons"):
                yield Button("Conectar", id="connect-button", variant="primary")
                yield Button("Demonstração", id="demo-button")
                yield Button("Cancelar" if self.allow_cancel else "Sair", id="cancel-button")

    def on_mount(self) -> None:
        self._populate_ports()
        if self.initial_port:
            detected = {port.device for port in list_available_ports()}
            if self.initial_port not in detected:
                self.query_one("#manual-port", Input).value = self.initial_port
        self.query_one("#port-list", OptionList).focus()

    def _populate_ports(self) -> None:
        widget = self.query_one("#port-list", OptionList)
        widget.clear_options()
        ports = list_available_ports()
        if not ports:
            widget.add_option(Option("Nenhuma porta detectada. Verifique o cabo USB.", id="__none__", disabled=True))
            return
        selected_index = 0
        for index, port in enumerate(ports):
            widget.add_option(Option(port.label(), id=port.device))
            if port.device == self.initial_port:
                selected_index = index
        widget.highlighted = selected_index

    def _selected_mode(self) -> str:
        value = self.query_one("#connection-mode", Select).value
        return str(value) if isinstance(value, str) else "auto"

    def _selected_port(self) -> str:
        manual = self.query_one("#manual-port", Input).value.strip()
        if manual:
            return manual
        widget = self.query_one("#port-list", OptionList)
        if widget.option_count == 0 or widget.highlighted is None:
            return ""
        option = widget.get_option_at_index(widget.highlighted)
        return "" if option.id == "__none__" else str(option.id or "")

    def action_refresh_ports(self) -> None:
        self._populate_ports()
        self.notify("Lista de portas atualizada", timeout=2)

    def action_cancel(self) -> None:
        self.dismiss(None)

    def action_demo(self) -> None:
        self.dismiss(ConnectionChoice(demo=True, mode="gateway"))

    @on(OptionList.OptionSelected, "#port-list")
    def _port_chosen(self) -> None:
        self._connect()

    @on(Button.Pressed, "#connect-button")
    def _connect(self) -> None:
        port = self._selected_port()
        if not port:
            self.notify("Escolha uma porta da lista ou informe o caminho.", severity="warning")
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


class ConfirmScreen(ModalScreen[bool]):
    BINDINGS = [
        Binding("escape", "no", "Não", show=False),
        Binding("n", "no", "Não", show=False),
        Binding("y", "yes", "Sim", show=False),
        Binding("s", "yes", "Sim", show=False),
    ]

    def __init__(self, title: str, message: str, *, confirm_label: str = "Confirmar", danger: bool = False) -> None:
        super().__init__()
        self.dialog_title = title
        self.message = message
        self.confirm_label = confirm_label
        self.danger = danger

    def compose(self) -> ComposeResult:
        with VerticalScroll(id="confirm-dialog", classes="dialog"):
            yield Static(self.dialog_title, classes="dialog-title")
            yield Static(self.message, id="confirm-message")
            with Horizontal(classes="dialog-buttons"):
                yield Button(self.confirm_label, id="confirm-yes", variant="error" if self.danger else "primary")
                yield Button("Cancelar", id="confirm-no")
            yield Static("S confirma, N ou Esc cancela.", classes="dialog-hint")

    def on_mount(self) -> None:
        self.query_one("#confirm-no", Button).focus()

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


class ParameterScreen(ModalScreen[str | None]):
    """Coleta o único parâmetro de uma ação do catálogo."""

    BINDINGS = [Binding("escape", "cancel", "Cancelar", show=False)]

    def __init__(self, action: CommandAction, target_label: str, current: str | None = None) -> None:
        super().__init__()
        self.action_item = action
        self.target_label = target_label
        self.current = current

    def compose(self) -> ComposeResult:
        parameter = self.action_item.parameter
        assert parameter is not None
        with VerticalScroll(id="parameter-dialog", classes="dialog"):
            yield Static(self.action_item.title, classes="dialog-title")
            yield Static(f"Alvo: {self.target_label}", classes="dialog-hint")
            if parameter.help:
                yield Static(parameter.help, classes="dialog-hint")
            label = parameter.label + (f" [{parameter.unit}]" if parameter.unit else "")
            yield Label(label)
            if parameter.kind == "choice":
                yield OptionList(*[Option(choice.label, id=choice.value) for choice in parameter.choices], id="parameter-choice")
            else:
                limits = ""
                if parameter.minimum is not None and parameter.maximum is not None:
                    limits = f"de {parameter.minimum} a {parameter.maximum}"
                yield Input(value=self.current or parameter.default, placeholder=limits, id="parameter-value")
                if limits:
                    yield Static(f"Valor aceito: {limits} {parameter.unit}".strip(), classes="dialog-hint")
            yield Static("", id="parameter-error")
            with Horizontal(classes="dialog-buttons"):
                yield Button("Enviar", id="parameter-ok", variant="primary")
                yield Button("Cancelar", id="parameter-cancel")

    def on_mount(self) -> None:
        parameter = self.action_item.parameter
        assert parameter is not None
        if parameter.kind == "choice":
            widget = self.query_one("#parameter-choice", OptionList)
            values = [choice.value for choice in parameter.choices]
            default = self.current if self.current in values else parameter.default
            widget.highlighted = values.index(default) if default in values else 0
            widget.focus()
        else:
            self.query_one("#parameter-value", Input).focus()

    def _value(self) -> str | None:
        parameter = self.action_item.parameter
        assert parameter is not None
        error = self.query_one("#parameter-error", Static)
        if parameter.kind == "choice":
            widget = self.query_one("#parameter-choice", OptionList)
            if widget.highlighted is None:
                return None
            return str(widget.get_option_at_index(widget.highlighted).id)
        text = self.query_one("#parameter-value", Input).value.strip()
        try:
            value = int(text)
        except ValueError:
            error.update(Text("Digite um número inteiro.", style=palette.STATE_WARNING))
            return None
        if parameter.minimum is not None and value < parameter.minimum:
            error.update(Text(f"O mínimo é {parameter.minimum} {parameter.unit}.", style=palette.STATE_WARNING))
            return None
        if parameter.maximum is not None and value > parameter.maximum:
            error.update(Text(f"O máximo é {parameter.maximum} {parameter.unit}.", style=palette.STATE_WARNING))
            return None
        return str(value)

    @on(OptionList.OptionSelected, "#parameter-choice")
    @on(Input.Submitted, "#parameter-value")
    @on(Button.Pressed, "#parameter-ok")
    def _ok(self) -> None:
        value = self._value()
        if value is not None:
            self.dismiss(value)

    @on(Button.Pressed, "#parameter-cancel")
    def _cancel_button(self) -> None:
        self.dismiss(None)

    def action_cancel(self) -> None:
        self.dismiss(None)


class ConfigScreen(ModalScreen[ConfigRequest | None]):
    """Formulário de configuração de aquisição de um sensor de vibração."""

    BINDINGS = [Binding("escape", "cancel", "Cancelar", show=False)]

    MODE_LABELS = {
        "IDLE": "Ocioso",
        "ROTATING": "Máquina rotativa",
        "STRUCTURAL": "Estrutural",
        "SEISMIC": "Sísmico (sem FFT)",
    }

    def __init__(self, sensor: SensorNode) -> None:
        super().__init__()
        self.sensor = sensor

    def compose(self) -> ComposeResult:
        cfg = self.sensor.configuration
        mode = cfg.mode.value if cfg.mode.value in commands.DEFAULT_FSM_MODES else "STRUCTURAL"
        window = (cfg.window_type or "HANN").upper()
        if window not in commands.DEFAULT_WINDOW_TYPES:
            window = "HANN"
        size = cfg.window_size if cfg.window_size in commands.DEFAULT_WINDOW_SIZES else 512
        with VerticalScroll(id="config-dialog", classes="dialog"):
            yield Static(f"Configurar aquisição do sensor {self.sensor.logical_id}", classes="dialog-title")
            yield Static(
                "Os valores são enviados ao firmware, aplicados juntos e confirmados pelo sensor.",
                classes="dialog-hint",
            )
            yield Label("Modo de operação")
            yield Select([(self.MODE_LABELS[item], item) for item in commands.DEFAULT_FSM_MODES], value=mode, allow_blank=False, id="cfg-mode")
            yield Label("Taxa de amostragem [Hz] (4 a 1000)")
            yield Input(value=_number(cfg.sample_rate_requested_hz), placeholder="ex.: 1000", id="cfg-rate")
            yield Label("Janela espectral")
            yield Select([(item.title(), item) for item in commands.DEFAULT_WINDOW_TYPES], value=window, allow_blank=False, id="cfg-window")
            yield Label("Tamanho da janela [amostras]")
            yield Select([(str(item), item) for item in commands.DEFAULT_WINDOW_SIZES], value=size, allow_blank=False, id="cfg-size")
            yield Label("Limiar do gatilho STA/LTA")
            yield Input(value=_number(cfg.stalta_threshold), placeholder="ex.: 4", id="cfg-stalta")
            yield Label("Ganho de calibração")
            yield Input(value=_number(cfg.calibration_gain), placeholder="ex.: 1", id="cfg-gain")
            yield Static("", id="cfg-error")
            with Horizontal(classes="dialog-buttons"):
                yield Button("Aplicar e verificar", id="cfg-verify", variant="primary")
                yield Button("Aplicar", id="cfg-apply")
                yield Button("Cancelar", id="cfg-cancel")

    def _build(self, verify: bool) -> ConfigRequest | None:
        error = self.query_one("#cfg-error", Static)
        try:
            rate = _float_or_none(self.query_one("#cfg-rate", Input).value)
            if rate is not None and not 4 <= rate <= 1000:
                raise ValueError("A taxa de amostragem deve ficar entre 4 e 1000 Hz.")
            stalta = _float_or_none(self.query_one("#cfg-stalta", Input).value)
            gain = _float_or_none(self.query_one("#cfg-gain", Input).value)
            if gain is not None and gain <= 0:
                raise ValueError("O ganho deve ser maior que zero.")
        except ValueError as exc:
            message = str(exc) if "deve" in str(exc) else "Use apenas números (vírgula ou ponto como separador)."
            error.update(Text(message, style=palette.STATE_WARNING))
            return None
        return ConfigRequest(
            mode=str(self.query_one("#cfg-mode", Select).value),
            window=str(self.query_one("#cfg-window", Select).value),
            rate_hz=rate,
            window_size=int(self.query_one("#cfg-size", Select).value),  # type: ignore[arg-type]
            stalta=stalta,
            gain=gain,
            apply_and_verify=verify,
        )

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


class TargetScreen(ModalScreen[str | None]):
    """Escolha do alvo dos comandos (rede, módulo ou sensor)."""

    BINDINGS = [Binding("escape", "cancel", "Cancelar", show=False)]

    def __init__(self, targets: list[tuple[str, str]], current: str) -> None:
        super().__init__()
        self.targets = targets
        self.current = current

    def compose(self) -> ComposeResult:
        with VerticalScroll(id="target-dialog", classes="dialog"):
            yield Static("Enviar comandos para", classes="dialog-title")
            yield OptionList(*[Option(label, id=value) for label, value in self.targets], id="target-list")
            yield Static("Enter escolhe, Esc cancela.", classes="dialog-hint")

    def on_mount(self) -> None:
        widget = self.query_one("#target-list", OptionList)
        values = [value for _label, value in self.targets]
        widget.highlighted = values.index(self.current) if self.current in values else 0
        widget.focus()

    @on(OptionList.OptionSelected, "#target-list")
    def _chosen(self, event: OptionList.OptionSelected) -> None:
        self.dismiss(str(event.option.id))

    def action_cancel(self) -> None:
        self.dismiss(None)


class WirelessNodeScreen(ModalScreen[int | None]):
    """Escolhe explicitamente qual módulo CAN assumirá um candidato wireless."""

    BINDINGS = [Binding("escape", "cancel", "Cancelar", show=False)]

    def __init__(self, wireless_uuid: str, profile: str, observations: list[tuple[int, int]]) -> None:
        super().__init__()
        self.wireless_uuid = wireless_uuid
        self.profile = profile
        self.observations = sorted(observations, key=lambda item: item[1], reverse=True)

    def compose(self) -> ComposeResult:
        with VerticalScroll(id="wireless-node-dialog", classes="dialog"):
            yield Static("Escolher módulo responsável", classes="dialog-title")
            yield Static(
                f"Sensor {self.wireless_uuid} · {self.profile}. "
                "A recomendação usa apenas a recepção BLE atual; o operador pode escolher outro módulo.",
                classes="dialog-hint",
            )
            options = []
            for index, (node_id, rssi) in enumerate(self.observations):
                suffix = " · melhor recepção" if index == 0 else ""
                options.append(Option(f"Módulo {node_id:02d} · {rssi} dBm{suffix}", id=str(node_id)))
            yield OptionList(*options, id="wireless-node-list")
            yield Static("Enter escolhe o módulo. Esc cancela.", classes="dialog-hint")

    def on_mount(self) -> None:
        widget = self.query_one("#wireless-node-list", OptionList)
        if widget.option_count:
            widget.highlighted = 0
            widget.focus()

    @on(OptionList.OptionSelected, "#wireless-node-list")
    def _chosen(self, event: OptionList.OptionSelected) -> None:
        try:
            self.dismiss(int(str(event.option.id)))
        except (TypeError, ValueError):
            self.dismiss(None)

    def action_cancel(self) -> None:
        self.dismiss(None)


class ChoiceScreen(ModalScreen[str | None]):
    """Escolha de uma opção em lista curta (política de reassociação, módulo de um ensaio)."""

    BINDINGS = [Binding("escape", "cancel", "Cancelar", show=False)]

    def __init__(self, title: str, hint: str, options: list[tuple[str, str]], current: str | None = None) -> None:
        super().__init__()
        self.dialog_title = title
        self.hint = hint
        self.options = options      # (rótulo, valor)
        self.current = current

    def compose(self) -> ComposeResult:
        with VerticalScroll(id="choice-dialog", classes="dialog"):
            yield Static(self.dialog_title, classes="dialog-title")
            if self.hint:
                yield Static(self.hint, classes="dialog-hint")
            yield OptionList(*[Option(label, id=value) for label, value in self.options], id="choice-list")
            yield Static("Enter escolhe, Esc cancela.", classes="dialog-hint")

    def on_mount(self) -> None:
        widget = self.query_one("#choice-list", OptionList)
        values = [value for _label, value in self.options]
        if widget.option_count:
            widget.highlighted = values.index(self.current) if self.current in values else 0
        widget.focus()

    @on(OptionList.OptionSelected, "#choice-list")
    def _chosen(self, event: OptionList.OptionSelected) -> None:
        self.dismiss(str(event.option.id))

    def action_cancel(self) -> None:
        self.dismiss(None)
