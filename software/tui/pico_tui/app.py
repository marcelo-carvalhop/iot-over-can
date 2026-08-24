from __future__ import annotations

import asyncio
import json
import math
import shlex
import time
import uuid
from dataclasses import asdict, is_dataclass
from datetime import datetime
from enum import Enum
from pathlib import Path
from typing import Any, Optional

from textual import on, work
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.events import Key, Resize
from textual.css.query import NoMatches
from textual.theme import Theme
from textual.widgets import Footer, Header, Input, Static, Tree

from pico_tui import commands, palette
from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    CommandAck,
    ConnectionClosed,
    ConnectionModeDetected,
    ConnectionOpened,
    DtcReceived,
    LogEvent,
    SpectrumReceived,
    TransferCompleted,
    TransferFailed,
)
from pico_tui.core.models import (
    ConnectionMode,
    AcquisitionMode,
    ConnectionState,
    DataQuality,
    SensorMode,
    SpectrumSample,
)
from pico_tui.core.state_store import StateStore
from pico_tui.dtc_catalog import dtc_description
from pico_tui.protocol.router import DecoderRouter
from pico_tui.screens import (
    ConfigRequest,
    ConfigScreen,
    ConfirmScreen,
    ConnectScreen,
    DtcScreen,
    FftRequestScreen,
    FftViewScreen,
    HelpScreen,
    MainMenuScreen,
    NodeNavigatorScreen,
    NetworkScreen,
    NodeDetailScreen,
    TelemetryCommandRequested,
    TelemetryScreen,
)
from pico_tui.serial_client import SerialClient
from pico_tui.security import SecurityManager
from pico_tui.services.controller import DomainController
from pico_tui.services.demo import DemoProducer
from pico_tui.services.log_manager import LogManager
from pico_tui.widgets import (
    ConfigPanel,
    EventLog,
    ModeSelected,
    QuickApplyRequested,
    NetworkTreePanel,
    QuickStatusPanel,
    TelemetryPanel,
    WindowSelected,
)


def build_theme() -> Theme:
    return Theme(
        name="tundra-autumn",
        primary=palette.ACCENT_FOCUS,
        secondary=palette.ACCENT_COPPER,
        accent=palette.ACCENT_ACTION,
        warning=palette.STATE_WARNING,
        error=palette.STATE_CRITICAL,
        success=palette.STATE_OK,
        foreground=palette.TEXT_PRIMARY,
        background=palette.BG_ROOT,
        surface=palette.BG_PANEL,
        panel=palette.BG_RAISED,
        dark=True,
    )


class PicoTuiApp(App[None]):
    CSS_PATH = "app.tcss"
    TITLE = "Wireless Sensor / CAN FD — Engineering TUI"
    SUB_TITLE = "desconectado"

    BINDINGS = [
        Binding("f1", "show_help", "Ajuda", show=True),
        Binding("f2", "show_menu", "Menu", show=True),
        Binding("f3", "show_node_navigator", "Selecionar nó", show=True),
        Binding("f4", "configure_selected", "Configurar", show=True),
        Binding("f5", "request_status", "Status", show=True),
        Binding("f6", "show_telemetry", "Telemetria", show=True),
        Binding("f7", "request_fft", "FFT", show=True),
        Binding("f8", "show_dtc", "DTC", show=True),
        Binding("f9", "show_network", "Rede", show=True),
        Binding("f10", "exit_confirm", "Sair", show=True),
        Binding("f11", "toggle_compact", "Compacto", show=False),
        Binding("f12", "snapshot", "Snapshot", show=False),
        Binding("ctrl+t", "toggle_telemetry", "Telemetria", show=False),
        Binding("ctrl+f", "request_fft", "FFT", show=False),
        Binding("ctrl+d", "show_dtc", "DTC", show=False),
        Binding("ctrl+r", "reconnect", "Reconectar", show=False),
        Binding("ctrl+p", "pause_refresh", "Pausar", show=False),
        Binding("ctrl+g", "show_gateway", "Gateway", show=False),
        Binding("ctrl+n", "focus_nodes", "Nós", show=False),
        Binding("ctrl+e", "focus_events", "Eventos", show=False),
        Binding("ctrl+b", "show_node_detail", "Detalhe", show=False),
        Binding("ctrl+k", "clear_dtc", "Limpar DTC", show=False),
        Binding("ctrl+m", "toggle_simulate", "Simular", show=False),
        Binding("ctrl+o", "restart_acquisition", "Reiniciar aquisição", show=False),
        Binding("ctrl+w", "wifi_connect", "Wi-Fi ON", show=False),
        Binding("ctrl+y", "security_status", "Segurança", show=False),
        Binding("q", "exit_confirm", "Sair", show=False),
    ]

    def __init__(
        self,
        port: Optional[str] = None,
        baudrate: int = 115200,
        *,
        mode: str = "auto",
        demo: bool = False,
        enable_file_log: bool = True,
        security_mode: str = "presence",
        security_config: str | None = None,
    ) -> None:
        super().__init__()
        self.initial_port = port
        self.baudrate = baudrate
        self.requested_mode = mode
        self.demo_requested = demo
        self.enable_file_log = enable_file_log
        self.security = SecurityManager(security_mode, security_config)

        self.bus = EventBus()
        self.state_store = StateStore()
        self.controller = DomainController(self.bus, self.state_store)
        self.decoder = DecoderRouter(self.bus, requested_mode=mode)
        self.session_id = uuid.uuid4().hex[:12]
        self.state_store.set_session_id(self.session_id)
        self.log_manager = LogManager(
            self.bus,
            session_id=self.session_id,
            directory=Path.cwd() / "logs",
            enable_file=enable_file_log,
        )
        self.demo_producer = DemoProducer(self.bus)
        self.serial_client: SerialClient | None = None
        self._line_queue: asyncio.Queue[str] = asyncio.Queue(maxsize=5000)
        self._last_port = port or ""
        self._last_mode = mode
        self._last_ping_sent: float | None = None
        self._manual_compact: bool | None = None
        self._refresh_paused = False
        self._last_selected_id: str | None = None
        self._connecting = False
        self.log_messages: list[str] = []

        # Compatibilidade com o programa-base e os testes existentes.
        self.connected = False
        self.net_state = "UNKNOWN"
        self.fsm_mode = "—"
        self.telemetry_on = False
        self.simulate_on = False
        self.port_name = ""

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        yield Static(id="status-bar")
        with Horizontal(id="main-row"):
            with Vertical(id="left-col"):
                yield NetworkTreePanel()
                yield QuickStatusPanel()
            with Vertical(id="center-col"):
                yield TelemetryPanel()
                yield EventLog()
            with Vertical(id="right-col"):
                yield ConfigPanel()
        yield Input(
            placeholder="Comando direto ou :status, :node 20.01, :fft once 20.01 bins=64",
            id="command-input",
        )
        yield Footer()

    def on_mount(self) -> None:
        self.register_theme(build_theme())
        self.theme = "tundra-autumn"
        self._register_event_handlers()
        self.run_worker(self._process_lines(), group="decoder", exclusive=True)
        self.set_interval(0.35, self._refresh_ui)
        self.set_interval(1.0, self._expire_fragment_transfers)
        self._refresh_status_bar()
        self._boot()

    async def on_unmount(self) -> None:
        self.demo_producer.stop()
        await asyncio.to_thread(self._close_serial)
        self.log_manager.close()

    def _register_event_handlers(self) -> None:
        self.bus.subscribe(LogEvent, self._on_log_event)
        self.bus.subscribe(ConnectionModeDetected, self._on_mode_detected)
        self.bus.subscribe(CommandAck, self._on_command_ack)
        self.bus.subscribe(TransferCompleted, self._on_transfer_completed)
        self.bus.subscribe(TransferFailed, self._on_transfer_failed)
        self.bus.subscribe(DtcReceived, self._on_dtc_received)
        self.bus.subscribe(SpectrumReceived, self._on_spectrum_received)

    @work
    async def _boot(self) -> None:
        await self.bus.publish(LogEvent("INFO", f"Sessão {self.session_id} iniciada", "TUI"))
        if self.demo_requested:
            await self._start_demo()
            return
        if self.initial_port:
            await self._connect_serial(self.initial_port, self.requested_mode)
            return
        choice = await self.push_screen_wait(ConnectScreen())
        if choice is None:
            self.exit()
        elif choice.demo:
            await self._start_demo()
        else:
            await self._connect_serial(choice.port, choice.mode)

    async def _start_demo(self) -> None:
        self.connected = True
        self.port_name = "DEMO"
        self._last_port = "DEMO"
        self._last_mode = "gateway"
        self.state_store.set_connection(ConnectionState.READY, port="DEMO", mode=ConnectionMode.DEMO)
        await self.bus.publish(LogEvent("INFO", "Modo demonstração iniciado", "DEMO"))
        self.run_worker(self.demo_producer.run(), group="demo", exclusive=True)

    async def _connect_serial(self, port: str, mode: str) -> None:
        if self._connecting:
            return
        self._connecting = True
        self._close_serial()
        self.decoder.requested_mode = mode
        self.decoder.reset()
        self._last_port = port
        self._last_mode = mode
        self.state_store.set_connection(ConnectionState.CONNECTING, port=port, mode=ConnectionMode.UNKNOWN)
        client = SerialClient(port, self.baudrate)
        try:
            await asyncio.to_thread(client.open)
        except Exception as exc:
            self._connecting = False
            self.connected = False
            await self.bus.publish(LogEvent("ERROR", f"Falha ao abrir {port}: {exc}", "SERIAL"))
            self.state_store.set_connection(ConnectionState.DISCONNECTED, port=port)
            return
        self.serial_client = client
        self.connected = True
        self.port_name = port
        client.set_console_echo_filter(mode.lower() != "gateway")
        client.start_reading(
            on_line=lambda line: self.call_from_thread(self._enqueue_line, line),
            on_error=lambda error: self.call_from_thread(self._serial_error, error),
            on_disconnect=lambda: self.call_from_thread(self._serial_disconnected),
        )
        await self.bus.publish(ConnectionOpened(port))
        await self.bus.publish(LogEvent("INFO", f"Conectado a {port}", "SERIAL"))
        forced = {
            "gateway": ConnectionMode.GATEWAY_CAN,
            "gateway_can": ConnectionMode.GATEWAY_CAN,
            "sensor": ConnectionMode.SENSOR_DIRECT,
            "sensor_direct": ConnectionMode.SENSOR_DIRECT,
        }.get(mode.lower())
        if forced:
            await self.bus.publish(ConnectionModeDetected(forced))
        await self._send_probe(mode)
        self._connecting = False
        self.query_one("#command-input", Input).focus()

    async def _send_probe(self, mode: str) -> None:
        if mode.lower() in {"sensor", "sensor_direct"}:
            for command in ("VERSION", "STATUS", "GET"):
                self._send_raw(command)
                await asyncio.sleep(0.04)
        elif mode.lower() in {"gateway", "gateway_can"}:
            for command in ("GW_VERSION", "GW_STATUS"):
                self._send_raw(command)
                await asyncio.sleep(0.04)
        else:
            # O firmware direto responde a VERSION/STATUS; gateways devem
            # ignorá-los ou responder ERR sem efeito. Em seguida sondamos o gateway.
            for command in ("VERSION", "STATUS", "GW_VERSION", "GW_STATUS"):
                self._send_raw(command)
                await asyncio.sleep(0.06)

    def _close_serial(self) -> None:
        client = self.serial_client
        self.serial_client = None
        if client:
            client.close()

    def _enqueue_line(self, line: str) -> None:
        try:
            self._line_queue.put_nowait(line)
        except asyncio.QueueFull:
            self.state_store.increment_network(parse_errors=1)
            self.log_messages.append("Fila serial cheia; linha descartada")

    async def _process_lines(self) -> None:
        while True:
            line = await self._line_queue.get()
            try:
                await self.decoder.decode(line)
            except Exception as exc:
                self.state_store.increment_network(parse_errors=1)
                await self.bus.publish(LogEvent("ERROR", f"Falha ao decodificar '{line}': {exc}", "PROTOCOL"))
            finally:
                self._line_queue.task_done()

    def _serial_error(self, error: str) -> None:
        self.run_worker(self.bus.publish(LogEvent("ERROR", error, "SERIAL")))

    def _serial_disconnected(self) -> None:
        if not self.connected:
            return
        self.connected = False
        self.run_worker(self.bus.publish(ConnectionClosed(self.port_name, "serial disconnected")))
        self.run_worker(self.bus.publish(LogEvent("ERROR", "Conexão serial perdida", "SERIAL")))

    async def _on_log_event(self, event: LogEvent) -> None:
        timestamp = datetime.now().strftime("%H:%M:%S")
        self.log_messages.append(f"{timestamp} [{event.level}] {event.source}: {event.message}")
        if len(self.log_messages) > 3000:
            del self.log_messages[:1000]
        colors = {
            "DEBUG": palette.TEXT_DIM,
            "INFO": palette.STATE_INFO,
            "WARNING": palette.STATE_WARNING,
            "ERROR": palette.STATE_CRITICAL,
            "CRITICAL": palette.STATE_CRITICAL,
        }
        color = colors.get(event.level.upper(), palette.TEXT_PRIMARY)
        safe = event.message.replace("[", "\\[")
        try:
            self.query_one(EventLog).write(
                f"[{palette.TEXT_DIM}]{timestamp}[/] [{color}][{event.level.upper()}][/] "
                f"[{palette.TEXT_MUTED}]{event.source}[/] {safe}"
            )
        except Exception:
            pass

    async def _on_mode_detected(self, event: ConnectionModeDetected) -> None:
        if self.serial_client:
            self.serial_client.set_console_echo_filter(event.mode == ConnectionMode.SENSOR_DIRECT)
        await self.bus.publish(LogEvent("INFO", f"Modo detectado: {event.mode.value}", "PROTOCOL"))
        if event.mode == ConnectionMode.SENSOR_DIRECT and self._last_mode.lower() == "auto":
            for command in ("VERSION", "STATUS", "GET"):
                self._send_raw(command)
                await asyncio.sleep(0.03)

    async def _on_command_ack(self, event: CommandAck) -> None:
        tx = f" tx={event.transaction_id}" if event.transaction_id else ""
        self.state_store.set_last_action(f"{event.state}: {event.command}{tx}")
        target = str(event.payload.get("TARGET", "")) if event.payload else ""
        sensor = self.state_store.find_sensor(target) if target else self._selected_sensor()
        if sensor and event.state in {"STAGED", "QUEUED", "APPLIED", "VERIFIED", "FAILED", "REJECTED"}:
            self.state_store.update_sensor_configuration(
                sensor.parent_node_id,
                sensor.child_id,
                transaction_state=event.state,
            )
        await self.bus.publish(LogEvent("INFO", f"{event.state}: {event.command}{tx}", "COMMAND"))
        if event.state == "APPLIED" and self.state_store.snapshot().connection_mode == ConnectionMode.SENSOR_DIRECT:
            self._send_raw("STATUS")

    async def _on_transfer_completed(self, event: TransferCompleted) -> None:
        await self.bus.publish(
            LogEvent(
                "INFO",
                f"{event.transfer_type} 0x{event.transfer_id:04X} concluída ({len(event.payload)} bytes)",
                "FRAGMENT",
            )
        )

    async def _on_transfer_failed(self, event: TransferFailed) -> None:
        await self.bus.publish(
            LogEvent(
                "ERROR",
                f"{event.transfer_type} 0x{event.transfer_id:04X}: {event.reason}",
                "FRAGMENT",
            )
        )

    async def _on_dtc_received(self, event: DtcReceived) -> None:
        level = event.record.severity.value
        await self.bus.publish(
            LogEvent(
                level,
                f"{event.parent_node_id:02d}.{event.child_id:02d} DTC 0x{event.record.code:04X} — "
                f"{dtc_description(event.record.code)} — severity={event.record.severity.value}",
                "DTC",
            )
        )
        if event.record.severity.value == "CRITICAL":
            self.notify(f"DTC crítico 0x{event.record.code:04X}", severity="error", timeout=5)

    async def _on_spectrum_received(self, event: SpectrumReceived) -> None:
        await self.bus.publish(
            LogEvent("INFO", f"FFT recebida de {event.sample.logical_id}: {len(event.sample.magnitudes)} bins", "FFT")
        )

    def _refresh_ui(self) -> None:
        if self._refresh_paused:
            return
        self.state_store.refresh_freshness()
        if self.state_store.snapshot().selected_logical_id is None:
            first = self.state_store.first_sensor_id()
            if first:
                self.state_store.set_selected(first)
        state = self.state_store.snapshot()
        selected = self.state_store.find_sensor(state.selected_logical_id) if state.selected_logical_id else None
        self._sync_compatibility_fields(state, selected)

        # Em versões mais antigas do Textual, ``App.query_one`` pesquisa apenas
        # a tela atualmente ativa. Enquanto um modal está aberto, os widgets da
        # tela principal não fazem parte dessa árvore e ``NoMatches`` é normal.
        # O estado continua sendo atualizado e será renderizado no próximo ciclo
        # após o fechamento do modal.
        try:
            self.query_one(NetworkTreePanel).refresh_state(state)
            self.query_one(TelemetryPanel).show_sensor(selected)
            self.query_one(ConfigPanel).show_sensor(selected)
            self.query_one(QuickStatusPanel).refresh_state(state, selected)
            self._refresh_status_bar(state)
        except NoMatches:
            return

    def _expire_fragment_transfers(self) -> None:
        self.run_worker(
            self.controller.expire_fragment_transfers(),
            group="fragment-expiry",
            exclusive=True,
        )

    def _sync_compatibility_fields(self, state: Any, selected: Any) -> None:
        self.connected = state.connection_state not in {ConnectionState.DISCONNECTED, ConnectionState.INCOMPATIBLE}
        self.port_name = state.port
        if selected:
            self.fsm_mode = selected.sensor_mode.value
            self.net_state = selected.health.network_state
            self.telemetry_on = selected.health.telemetry_enabled
            self.simulate_on = selected.configuration.simulation_enabled

    def _refresh_status_bar(self, state: Any | None = None) -> None:
        state = state or self.state_store.snapshot()
        selected = state.selected_logical_id or "N/A"
        conn_color = palette.STATE_OK if self.connected else palette.STATE_CRITICAL
        paused = f"  [{palette.STATE_WARNING}]PAUSADO[/]" if self._refresh_paused else ""
        try:
            status_bar = self.query_one("#status-bar", Static)
        except NoMatches:
            return
        status_bar.update(
            f"[b {conn_color}]{state.connection_state.value}[/]  │  "
            f"{state.connection_mode.value}  │  porta={state.port or 'N/A'}  │  "
            f"alvo=[b]{selected}[/b]  │  CAN={state.gateway.can_state}  │  "
            f"{self.security.status_label()}  │  RX={state.network.frames_rx} CRC={state.network.crc_errors}{paused}"
        )
        self.sub_title = f"{state.connection_mode.value} | {selected} | sessão {self.session_id}"

    @on(Tree.NodeSelected, "#node-tree")
    def _node_selected(self, event: Tree.NodeSelected) -> None:
        data = event.node.data
        if isinstance(data, tuple) and data and data[0] == "sensor":
            logical_id = f"{int(data[1]):02d}.{int(data[2]):02d}"
            self.state_store.set_selected(logical_id)
            self._last_selected_id = logical_id

    @on(Input.Submitted, "#command-input")
    def _command_submitted(self, event: Input.Submitted) -> None:
        text = event.value.strip()
        event.input.value = ""
        if not text:
            return
        if text.startswith(":"):
            self.run_worker(self._execute_internal_command(text[1:]))
        else:
            self._send_raw(text)

    @on(ModeSelected)
    def _mode_selected(self, event: ModeSelected) -> None:
        self.run_worker(self._stage_field("MODE", event.mode))

    @on(WindowSelected)
    def _window_selected(self, event: WindowSelected) -> None:
        self.run_worker(self._stage_field("WINDOW", event.window))

    @on(QuickApplyRequested)
    def _quick_apply_requested(self) -> None:
        self.run_worker(self._apply_quick_configuration())

    def _send_raw(self, command: str) -> bool:
        if self.demo_requested or self.state_store.snapshot().connection_mode == ConnectionMode.DEMO:
            self.run_worker(self.bus.publish(LogEvent("INFO", f"> {command} [DEMO]", "COMMAND")))
            return True
        decision = self.security.authorize_command(command)
        if not decision.allowed:
            self.run_worker(self.bus.publish(LogEvent("ERROR", f"Bloqueado: {command} — {decision.reason}", "SECURITY")))
            try:
                self.notify(decision.reason, title="Segurança", severity="error")
            except Exception:
                pass
            return False

        client = self.serial_client
        if client is None or not client.is_open:
            self.run_worker(self.bus.publish(LogEvent("ERROR", f"Não conectado: {command}", "COMMAND")))
            return False
        try:
            client.write_line(command)
        except Exception as exc:
            self.run_worker(self.bus.publish(LogEvent("ERROR", f"Falha de envio: {exc}", "SERIAL")))
            return False
        self.state_store.set_last_action(command)
        self.run_worker(self.bus.publish(LogEvent("DEBUG", f"> {command}", "COMMAND")))
        if command.strip().upper() == "PING":
            self._last_ping_sent = time.monotonic()
        return True

    def _selected_sensor(self):
        state = self.state_store.snapshot()
        return self.state_store.find_sensor(state.selected_logical_id) if state.selected_logical_id else None

    async def _stage_field(self, field: str, value: object) -> None:
        sensor = self._selected_sensor()
        if sensor is None:
            await self.bus.publish(LogEvent("WARNING", "Selecione um sensor", "COMMAND"))
            return
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            self._send_raw(commands.direct_set(field, value))
        elif mode in {ConnectionMode.GATEWAY_CAN, ConnectionMode.DEMO}:
            command = commands.gateway_command(sensor.logical_id, "CONFIG", **{field: value}, STATE="QUEUED")
            self._send_raw(command)
            if mode == ConnectionMode.DEMO:
                if field == "MODE":
                    self.state_store.update_sensor_configuration(sensor.parent_node_id, sensor.child_id, mode=SensorMode(str(value)))
                elif field == "WINDOW":
                    self.state_store.update_sensor_configuration(sensor.parent_node_id, sensor.child_id, window_type=str(value))
        else:
            await self.bus.publish(LogEvent("WARNING", "Protocolo ainda não identificado", "COMMAND"))

    async def _apply_quick_configuration(self) -> None:
        sensor = self._selected_sensor()
        if sensor is None:
            await self.bus.publish(LogEvent("WARNING", "Selecione um sensor", "CONFIG"))
            self.notify("Selecione um sensor", severity="warning")
            return
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            self._send_raw(commands.direct_apply())
            self.state_store.update_sensor_configuration(
                sensor.parent_node_id,
                sensor.child_id,
                transaction_state="SENT",
            )
            await self.bus.publish(
                LogEvent("INFO", f"APPLY enviado para {sensor.logical_id}; aguardando CONFIG_APPLIED", "CONFIG")
            )
        elif mode == ConnectionMode.GATEWAY_CAN:
            self._send_raw(
                commands.gateway_command(
                    sensor.logical_id,
                    "CONFIG",
                    APPLY="YES",
                    VERIFY="YES",
                )
            )
            self.state_store.update_sensor_configuration(
                sensor.parent_node_id,
                sensor.child_id,
                transaction_state="SENT",
            )
            await self.bus.publish(
                LogEvent("INFO", f"Aplicação rápida solicitada para {sensor.logical_id}", "CONFIG")
            )
        elif mode == ConnectionMode.DEMO:
            self.state_store.update_sensor_configuration(
                sensor.parent_node_id,
                sensor.child_id,
                transaction_state="VERIFIED",
            )
            await self.bus.publish(LogEvent("INFO", f"Configuração rápida aplicada a {sensor.logical_id} [DEMO]", "CONFIG"))
        else:
            await self.bus.publish(LogEvent("WARNING", "Protocolo ainda não identificado", "CONFIG"))

    async def _execute_internal_command(self, command_line: str) -> None:
        try:
            parts = shlex.split(command_line)
        except ValueError as exc:
            await self.bus.publish(LogEvent("ERROR", str(exc), "COMMAND"))
            return
        if not parts:
            return
        cmd = parts[0].lower()
        args = parts[1:]
        if cmd == "status":
            await self._request_status()
        elif cmd == "node" and args:
            if self.state_store.find_sensor(args[0]):
                self.state_store.set_selected(args[0])
            else:
                await self.bus.publish(LogEvent("WARNING", f"Sensor {args[0]} não encontrado", "COMMAND"))
        elif cmd == "tel":
            await self._command_tel(args)
        elif cmd == "fft":
            await self._command_fft(args)
        elif cmd == "dtc":
            await self._command_dtc(args)
        elif cmd == "acq":
            await self._command_acq(args)
        elif cmd == "wifi":
            await self._command_wifi(args)
        elif cmd == "unlock" and args:
            decision = self.security.unlock_with_otp(args[0])
            await self.bus.publish(LogEvent("INFO" if decision.allowed else "ERROR", decision.reason, "SECURITY"))
        elif cmd == "lock":
            self.security.lock()
            await self.bus.publish(LogEvent("INFO", "TUI bloqueada pelo operador", "SECURITY"))
        elif cmd == "security":
            await self.bus.publish(LogEvent("INFO", self.security.status_label(), "SECURITY"))
        elif cmd == "config":
            await self._command_config(args)
        elif cmd == "disconnect":
            await self._disconnect()
        elif cmd == "reconnect":
            await self._reconnect()
        elif cmd == "export" and args and args[0].lower() == "csv":
            self._export_csv()
        elif cmd == "save" and args and args[0].lower() == "log":
            await self.bus.publish(LogEvent("INFO", "O log JSONL é persistido continuamente", "LOG"))
        elif cmd in {"quit", "exit"}:
            self.action_exit_confirm()
        elif cmd == "connect" and args:
            await self._connect_serial(args[0], "auto")
        else:
            await self.bus.publish(LogEvent("ERROR", f"Comando interno desconhecido: {cmd}", "COMMAND"))

    async def _command_tel(self, args: list[str]) -> None:
        if not args:
            await self.bus.publish(LogEvent("WARNING", "Uso: :tel on|off|once|fast|slow|period <ms>|rate", "COMMAND"))
            return
        action = args[0].lower()
        if action in {"on", "off"}:
            await self._set_telemetry(action == "on")
            return
        mode = self.state_store.snapshot().connection_mode
        if action in {"once", "fast", "slow"}:
            if mode == ConnectionMode.SENSOR_DIRECT:
                self._send_raw(commands.direct_telemetry(action))
            else:
                sensor = self._selected_sensor()
                if sensor:
                    self._send_raw(commands.gateway_command(sensor.logical_id, "TELEMETRY", STATE=action.upper()))
            return
        if action == "period" and len(args) > 1:
            try:
                period_ms = int(args[1])
                if period_ms <= 0:
                    raise ValueError
            except ValueError:
                await self.bus.publish(LogEvent("ERROR", "Período inválido", "COMMAND"))
                return
            if mode == ConnectionMode.SENSOR_DIRECT:
                self._send_raw(commands.direct_telemetry("PERIOD", period_ms))
            else:
                sensor = self._selected_sensor()
                if sensor:
                    self._send_raw(commands.gateway_command(sensor.logical_id, "TELEMETRY_PERIOD", PERIOD_MS=period_ms))
            return
        if action == "rate" and len(args) > 1:
            profile = args[1].upper()
            value = commands.TELEMETRY_PROFILES.get(profile)
            if value is None:
                try:
                    value = float(args[1])
                except ValueError:
                    await self.bus.publish(LogEvent("ERROR", "Taxa inválida", "COMMAND"))
                    return
            sensor = self._selected_sensor()
            if sensor:
                if mode == ConnectionMode.SENSOR_DIRECT:
                    if profile == "FAST":
                        self._send_raw(commands.direct_telemetry("FAST"))
                    elif profile in {"LOW", "SLOW"}:
                        self._send_raw(commands.direct_telemetry("SLOW"))
                    else:
                        period_ms = max(1, round(1000.0 / float(value)))
                        self._send_raw(commands.direct_telemetry("PERIOD", period_ms))
                else:
                    self._send_raw(commands.gateway_command(sensor.logical_id, "TELEMETRY_RATE", RATE_HZ=value))
            return
        await self.bus.publish(LogEvent("WARNING", "Uso: :tel on|off|once|fast|slow|period <ms>|rate normal", "COMMAND"))

    async def _command_fft(self, args: list[str]) -> None:
        target = next((arg for arg in args if "." in arg and "=" not in arg), None)
        if target and self.state_store.find_sensor(target):
            self.state_store.set_selected(target)
        bins = 64
        for arg in args:
            if arg.lower().startswith("bins="):
                try:
                    bins = int(arg.split("=", 1)[1])
                except ValueError:
                    pass
        await self._request_fft_for_selected(bins, "VIEW_ONLY")

    async def _command_dtc(self, args: list[str]) -> None:
        target = next((arg for arg in args if "." in arg and "=" not in arg), None)
        if target and self.state_store.find_sensor(target):
            self.state_store.set_selected(target)
        filtered = [arg for arg in args if arg != target]
        if not filtered or filtered[0].lower() == "list":
            await self._send_dtc_list()
            return
        if filtered[0].lower() == "clear":
            code: int | None = None
            for arg in filtered[1:]:
                if arg.lower() == "all":
                    code = None
                    break
                try:
                    code = int(arg, 0)
                    break
                except ValueError:
                    continue
            await self._send_dtc_clear(code)
            return
        await self.bus.publish(LogEvent("WARNING", "Uso: :dtc list [sensor] | :dtc clear [sensor] [all|0xCODE]", "COMMAND"))

    async def _command_acq(self, args: list[str]) -> None:
        if not args or args[0].lower() != "polling":
            await self.bus.publish(
                LogEvent(
                    "WARNING",
                    "Uso: :acq polling. ACQ DRDY não pertence à baseline atual.",
                    "COMMAND",
                )
            )
            return
        await self._restart_acquisition()

    async def _command_config(self, args: list[str]) -> None:
        target = next((arg for arg in args if "." in arg and "=" not in arg), None)
        if target and self.state_store.find_sensor(target):
            self.state_store.set_selected(target)
        fields: dict[str, str] = {}
        for arg in args:
            if "=" in arg:
                key, value = arg.split("=", 1)
                normalized_key = {"SIZE": "WINDOW_SIZE"}.get(key.upper(), key.upper())
                fields[normalized_key] = value.upper() if normalized_key in {"MODE", "WINDOW"} else value
        sensor = self._selected_sensor()
        if not sensor:
            return
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            direct_order = ("MODE", "RATE", "WINDOW", "WINDOW_SIZE", "STALTA", "GAIN")
            for field in direct_order:
                if field in fields:
                    self._send_raw(commands.direct_set(field, fields[field]))
                    await asyncio.sleep(0.03)
            self._send_raw(commands.direct_apply())
        else:
            self._send_raw(commands.gateway_command(sensor.logical_id, "CONFIG", **fields))

    async def _request_status(self) -> None:
        sensor = self._selected_sensor()
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            self._send_raw("STATUS")
        elif sensor:
            self._send_raw(commands.gateway_command(sensor.logical_id, "STATUS"))
        else:
            self._send_raw("GW_STATUS")

    async def _set_telemetry(self, enabled: bool) -> None:
        sensor = self._selected_sensor()
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            self._send_raw(commands.direct_telemetry("ON" if enabled else "OFF"))
        elif mode == ConnectionMode.DEMO and sensor:
            self.state_store.update_sensor_health(
                sensor.parent_node_id,
                sensor.child_id,
                telemetry_enabled=enabled,
            )
            await self.bus.publish(
                LogEvent("INFO", f"Telemetria demo {'ligada' if enabled else 'desligada'} em {sensor.logical_id}", "DEMO")
            )
        elif sensor:
            self._send_raw(commands.gateway_command(sensor.logical_id, "TELEMETRY", STATE="ON" if enabled else "OFF"))
        else:
            await self.bus.publish(LogEvent("WARNING", "Selecione um sensor", "COMMAND"))

    async def _execute_telemetry_control(self, action: str, value: int | None = None) -> None:
        sensor = self._selected_sensor()
        if sensor is None:
            await self.bus.publish(LogEvent("WARNING", "Selecione um sensor", "COMMAND"))
            return
        action = action.upper()
        if action in {"ON", "OFF"}:
            await self._set_telemetry(action == "ON")
            return
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            self._send_raw(commands.direct_telemetry(action, value))
        elif mode == ConnectionMode.DEMO:
            updates: dict[str, object] = {}
            if action == "FAST":
                updates["telemetry_period_ms"] = 200
            elif action == "SLOW":
                updates["telemetry_period_ms"] = 2000
            elif action == "PERIOD" and value is not None:
                updates["telemetry_period_ms"] = value
            if updates:
                self.state_store.update_sensor_health(sensor.parent_node_id, sensor.child_id, **updates)
            await self.bus.publish(LogEvent("INFO", f"Telemetria demo {action} {value or ''}".strip(), "DEMO"))
        else:
            fields: dict[str, object] = {"STATE": action}
            if action == "PERIOD" and value is not None:
                fields = {"PERIOD_MS": value}
                gateway_action = "TELEMETRY_PERIOD"
            else:
                gateway_action = "TELEMETRY"
            self._send_raw(commands.gateway_command(sensor.logical_id, gateway_action, **fields))

    async def _toggle_simulation(self) -> None:
        sensor = self._selected_sensor()
        if not sensor:
            return
        enabled = not sensor.configuration.simulation_enabled
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            self._send_raw(commands.direct_simulate(enabled))
        elif mode == ConnectionMode.DEMO:
            self.state_store.update_sensor_configuration(sensor.parent_node_id, sensor.child_id, simulation_enabled=enabled)
            self.state_store.update_sensor(sensor.parent_node_id, sensor.child_id, quality=DataQuality.SIMULATED if enabled else DataQuality.REAL)
        else:
            self._send_raw(commands.gateway_command(sensor.logical_id, "SIMULATE", STATE="ON" if enabled else "OFF"))

    async def _request_fft_for_selected(self, bins: int, fft_mode: str) -> bool:
        sensor = self._selected_sensor()
        if sensor is None:
            await self.bus.publish(LogEvent("WARNING", "Selecione um sensor", "FFT"))
            return False
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            if sensor.sensor_mode == SensorMode.SEISMIC:
                await self.bus.publish(
                    LogEvent("INFO", "FFT não solicitada: FFT_VALID=NO é esperado no modo SEISMIC", "FFT")
                )
                return False
            self._send_raw(commands.direct_fft_once())
            await asyncio.sleep(0.04)
            self._send_raw(commands.direct_telemetry("ONCE"))
            return True
        if mode == ConnectionMode.DEMO:
            values = [
                1000.0 * math.exp(-((index - bins * 0.22) ** 2) / (2 * (bins * 0.035) ** 2))
                + 300.0 * math.exp(-((index - bins * 0.61) ** 2) / (2 * (bins * 0.05) ** 2))
                for index in range(bins)
            ]
            await self.bus.publish(
                SpectrumReceived(
                    SpectrumSample(
                        sensor.parent_node_id,
                        sensor.child_id,
                        values,
                        transfer_id=1,
                        sample_rate_hz=sensor.configuration.sample_rate_effective_hz,
                        fft_size=bins * 2,
                        window_type=sensor.configuration.window_type,
                    )
                )
            )
            return True
        self._send_raw(commands.gateway_command(sensor.logical_id, "FFT", MODE=fft_mode, BINS=bins))
        return True

    async def _send_dtc_list(self) -> None:
        sensor = self._selected_sensor()
        if sensor is None:
            return
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            self._send_raw("DTC")
        elif mode == ConnectionMode.DEMO:
            await self.bus.publish(LogEvent("INFO", "Lista DTC obtida do estado demo", "DTC"))
        else:
            self._send_raw(commands.gateway_command(sensor.logical_id, "DTC_LIST"))

    async def _send_dtc_clear(self, code: int | None) -> None:
        sensor = self._selected_sensor()
        if sensor is None:
            return
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            # A baseline direta oferece limpeza global com o comando literal DTC CLEAR.
            self._send_raw(commands.direct_dtc_clear())
        elif mode == ConnectionMode.DEMO:
            self.state_store.clear_dtc(sensor.parent_node_id, sensor.child_id, code)
        else:
            fields: dict[str, object] = {"SCOPE": "ALL" if code is None else "ONE"}
            if code is not None:
                fields["CODE"] = f"0x{code:04X}"
            self._send_raw(commands.gateway_command(sensor.logical_id, "DTC_CLEAR", **fields))

    async def _restart_acquisition(self) -> None:
        sensor = self._selected_sensor()
        if sensor is None:
            await self.bus.publish(LogEvent("WARNING", "Selecione um sensor", "COMMAND"))
            return
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            self._send_raw(commands.direct_acq_polling())
        elif mode == ConnectionMode.DEMO:
            self.state_store.update_sensor(
                sensor.parent_node_id,
                sensor.child_id,
                acquisition_mode=AcquisitionMode.POLLING,
            )
            await self.bus.publish(LogEvent("INFO", "Aquisição POLLING reiniciada no modo demo", "ACQ"))
        else:
            self._send_raw(commands.gateway_command(sensor.logical_id, "ACQ", MODE="POLLING"))

    async def _set_wifi(self, enabled: bool) -> None:
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            self._send_raw(commands.direct_wifi("ON" if enabled else "OFF"))
        elif mode == ConnectionMode.DEMO:
            sensor = self._selected_sensor()
            if sensor:
                self.state_store.update_sensor_health(
                    sensor.parent_node_id,
                    sensor.child_id,
                    network_state="DISCOVERY" if enabled else "DISABLED",
                )
            await self.bus.publish(LogEvent("INFO", f"Wi-Fi demo {'ON' if enabled else 'OFF'}", "NET"))
        else:
            sensor = self._selected_sensor()
            target = sensor.logical_id if sensor else "GATEWAY"
            self._send_raw(commands.gateway_command(target, "WIFI", STATE="ON" if enabled else "OFF"))

    async def _wifi_status(self) -> None:
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            self._send_raw(commands.direct_wifi("STATUS"))
        elif mode == ConnectionMode.DEMO:
            await self.bus.publish(LogEvent("INFO", "Wi-Fi demo: estado mantido no snapshot", "NET"))
        else:
            sensor = self._selected_sensor()
            target = sensor.logical_id if sensor else "GATEWAY"
            self._send_raw(commands.gateway_command(target, "WIFI_STATUS"))

    async def _stop_telemetry_stream(self) -> None:
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            client = self.serial_client
            if client is None or not client.is_open:
                await self.bus.publish(LogEvent("ERROR", "Porta serial desconectada", "COMMAND"))
                return
            try:
                client.write_raw(b"\x03")
                await self.bus.publish(LogEvent("INFO", "> Ctrl+C (0x03)", "COMMAND"))
            except Exception:
                self._send_raw("!")
        else:
            await self._set_telemetry(False)

    async def _disconnect(self) -> None:
        self._close_serial()
        self.connected = False
        self.state_store.set_connection(ConnectionState.DISCONNECTED)
        await self.bus.publish(LogEvent("INFO", "Desconectado pelo operador", "SERIAL"))

    async def _reconnect(self) -> None:
        if not self._last_port or self._last_port == "DEMO":
            await self.bus.publish(LogEvent("WARNING", "Nenhuma porta anterior para reconectar", "SERIAL"))
            return
        self.state_store.set_connection(ConnectionState.RECONNECTING, port=self._last_port)
        await self._connect_serial(self._last_port, self._last_mode)

    def _export_csv(self) -> None:
        path = Path.cwd() / "exports" / f"telemetry_{self.session_id}.csv"
        output = self.log_manager.export_telemetry_csv(path)
        self.run_worker(self.bus.publish(LogEvent("INFO", f"CSV exportado: {output}", "EXPORT")))

    def _snapshot(self) -> Path:
        output = Path.cwd() / "exports" / f"snapshot_{datetime.now():%Y%m%d_%H%M%S}.json"
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(_jsonable(self.state_store.snapshot()), ensure_ascii=False, indent=2), encoding="utf-8")
        return output

    def action_show_help(self) -> None:
        self.push_screen(HelpScreen())

    @work
    async def action_show_menu(self) -> None:
        action = await self.push_screen_wait(MainMenuScreen())
        if not action:
            return
        mapping = {
            "connection": self.action_reconnect,
            "nodes": self.action_show_node_navigator,
            "telemetry": self.action_show_telemetry,
            "fft": self.action_request_fft,
            "dtc": self.action_show_dtc,
            "config": self.action_configure_selected,
            "network": self.action_show_network,
            "wifi_on": self.action_wifi_connect,
            "wifi_off": lambda: self.run_worker(self._set_wifi(False)),
            "gateway": self.action_show_gateway,
            "logs": self.action_focus_events,
            "help": self.action_show_help,
            "quit": self.action_exit_confirm,
        }
        callback = mapping.get(action)
        if callback:
            callback()

    @work
    async def action_show_node_navigator(self) -> None:
        state = self.state_store.snapshot()
        logical_id = await self.push_screen_wait(
            NodeNavigatorScreen(state, state.selected_logical_id)
        )
        if logical_id:
            self.state_store.set_selected(logical_id)
            self._last_selected_id = logical_id
            await self.bus.publish(LogEvent("INFO", f"Sensor selecionado: {logical_id}", "NAVIGATION"))
            try:
                self.query_one("#node-tree", Tree).focus()
            except NoMatches:
                pass

    def action_focus_nodes(self) -> None:
        self.query_one("#node-tree", Tree).focus()

    def action_show_telemetry(self) -> None:
        sensor = self._selected_sensor()
        if sensor is None:
            self.notify("Selecione um sensor", severity="warning")
            return
        self.push_screen(TelemetryScreen(sensor.logical_id, self.state_store.find_sensor))

    # Compatibilidade com chamadas internas e versões anteriores.
    def action_focus_telemetry(self) -> None:
        self.action_show_telemetry()

    def action_focus_events(self) -> None:
        self.query_one("#event-log").focus()

    def action_request_status(self) -> None:
        self.run_worker(self._request_status())

    def action_toggle_telemetry(self) -> None:
        self.run_worker(self._set_telemetry(not self.telemetry_on))

    def action_toggle_simulate(self) -> None:
        self.run_worker(self._toggle_simulation())

    @on(TelemetryCommandRequested)
    def _telemetry_command_requested(self, event: TelemetryCommandRequested) -> None:
        self.state_store.set_selected(event.logical_id)
        self.run_worker(self._execute_telemetry_control(event.action, event.value))

    @work
    async def action_configure_selected(self) -> None:
        sensor = self._selected_sensor()
        if sensor is None:
            self.notify("Selecione um sensor", severity="warning")
            return
        request = await self.push_screen_wait(ConfigScreen(sensor))
        if request:
            await self._apply_config_request(sensor.logical_id, request)

    async def _apply_config_request(self, logical_id: str, request: ConfigRequest) -> None:
        sensor = self.state_store.find_sensor(logical_id)
        if not sensor:
            return
        mode = self.state_store.snapshot().connection_mode
        if mode == ConnectionMode.SENSOR_DIRECT:
            commands_to_send = [
                commands.direct_set("MODE", request.mode),
                commands.direct_set("RATE", request.rate_hz) if request.rate_hz is not None else None,
                commands.direct_set("WINDOW", request.window),
                commands.direct_set("WINDOW_SIZE", request.window_size) if request.window_size is not None else None,
                commands.direct_set("STALTA", request.stalta) if request.stalta is not None else None,
                commands.direct_set("GAIN", request.gain) if request.gain is not None else None,
                commands.direct_apply(),
            ]
            for command in commands_to_send:
                if command:
                    self._send_raw(command)
                    await asyncio.sleep(0.03)
            # A verificação será disparada somente após CONFIG_APPLIED.
        elif mode == ConnectionMode.DEMO:
            self.state_store.update_sensor_configuration(
                sensor.parent_node_id,
                sensor.child_id,
                mode=SensorMode(request.mode),
                sample_rate_requested_hz=request.rate_hz,
                sample_rate_effective_hz=request.rate_hz,
                window_type=request.window,
                window_size=request.window_size,
                stalta_threshold=request.stalta,
                calibration_gain=request.gain,
                transaction_state="VERIFIED" if request.apply_and_verify else "APPLIED",
            )
            await self.bus.publish(LogEvent("INFO", f"Configuração demo aplicada a {logical_id}", "CONFIG"))
        else:
            self._send_raw(
                commands.gateway_command(
                    logical_id,
                    "CONFIG",
                    MODE=request.mode,
                    RATE_HZ=request.rate_hz,
                    WINDOW=request.window,
                    WINDOW_SIZE=request.window_size,
                    STALTA=request.stalta,
                    GAIN=request.gain,
                    VERIFY="YES" if request.apply_and_verify else "NO",
                )
            )

    @work
    async def action_request_fft(self) -> None:
        sensor = self._selected_sensor()
        if sensor is None:
            self.notify("Selecione um sensor", severity="warning")
            return
        request = await self.push_screen_wait(FftRequestScreen(sensor.logical_id))
        if request:
            requested_at = time.time()
            sent = await self._request_fft_for_selected(request.bins, request.mode)
            if sent:
                self.push_screen(
                    FftViewScreen(
                        sensor.logical_id,
                        self.state_store.find_sensor,
                        requested_at=requested_at,
                    )
                )

    @work
    async def action_show_dtc(self) -> None:
        sensor = self._selected_sensor()
        if sensor is None:
            self.notify("Selecione um sensor", severity="warning")
            return
        result = await self.push_screen_wait(DtcScreen(sensor))
        if result == "all":
            confirmed = await self.push_screen_wait(
                ConfirmScreen("LIMPAR DTCs", f"Apagar todos os DTCs ativos do sensor {sensor.logical_id}?")
            )
            if confirmed:
                await self._send_dtc_clear(None)
        elif result and result.startswith("one:"):
            code_text = result.split(":", 1)[1]
            try:
                code = int(code_text, 0)
            except ValueError:
                code = int(code_text)
            await self._send_dtc_clear(code)

    def action_show_network(self) -> None:
        self.push_screen(NetworkScreen(self.state_store.snapshot()))

    def action_show_gateway(self) -> None:
        state = self.state_store.snapshot()
        message = (
            f"Node {state.gateway.node_id:02d}\nFirmware: {state.gateway.firmware_version or 'N/A'}\n"
            f"Protocolo: {state.gateway.protocol_version or 'N/A'}\nCAN: {state.gateway.can_state}\n"
            f"Wi-Fi: {state.gateway.wifi_state}\nPorta: {state.gateway.serial_port or state.port or 'N/A'}"
        )
        self.notify(message, title="Gateway", timeout=6)

    def action_clear_dtc(self) -> None:
        self.action_show_dtc()

    def action_show_node_detail(self) -> None:
        sensor = self._selected_sensor()
        if sensor is None:
            self.notify("Selecione um sensor", severity="warning")
            return
        self.push_screen(NodeDetailScreen(sensor))

    def action_restart_acquisition(self) -> None:
        self.run_worker(self._restart_acquisition())

    def action_wifi_connect(self) -> None:
        self.run_worker(self._set_wifi(True))

    def action_security_status(self) -> None:
        self.notify(self.security.status_label(), title="Segurança", timeout=5)

    def action_stop_telemetry(self) -> None:
        self.run_worker(self._stop_telemetry_stream())

    def action_toggle_compact(self) -> None:
        current = self.has_class("compact")
        self._manual_compact = not current
        self.set_class(not current, "compact")

    def action_pause_refresh(self) -> None:
        self._refresh_paused = not self._refresh_paused
        self.state_store.set_refresh_paused(self._refresh_paused)
        self._refresh_status_bar()

    def action_snapshot(self) -> None:
        output = self._snapshot()
        self.run_worker(self.bus.publish(LogEvent("INFO", f"Snapshot salvo: {output}", "EXPORT")))

    def action_reconnect(self) -> None:
        self.run_worker(self._reconnect())

    @work
    async def action_exit_confirm(self) -> None:
        confirmed = await self.push_screen_wait(ConfirmScreen("ENCERRAR", "Deseja encerrar a TUI?"))
        if confirmed:
            self.exit()

    def on_key(self, event: Key) -> None:
        # Textual 1.x reserva Ctrl+C antes do sistema de bindings. O tratamento
        # explícito mantém o mesmo comportamento nas versões 1.x e 8.x.
        if event.key == "ctrl+c":
            event.prevent_default()
            event.stop()
            self.action_stop_telemetry()

    def on_resize(self, event: Resize) -> None:
        if self._manual_compact is None:
            self.set_class(event.size.width < 112, "compact")


def _jsonable(value: Any) -> Any:
    if isinstance(value, Enum):
        return value.value
    if isinstance(value, bytes):
        return value.hex()
    if is_dataclass(value):
        return {key: _jsonable(item) for key, item in asdict(value).items()}
    if isinstance(value, dict):
        return {str(key): _jsonable(item) for key, item in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [_jsonable(item) for item in value]
    if hasattr(value, "__iter__") and value.__class__.__name__ == "deque":
        return [_jsonable(item) for item in value]
    return value
