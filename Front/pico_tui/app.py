from __future__ import annotations

import asyncio
import json
import math
import shlex
import time
import uuid
from collections import deque
from dataclasses import asdict, is_dataclass
from datetime import datetime
from enum import Enum
from pathlib import Path
from typing import Any, Optional

from textual import work
from textual.app import App
from textual.binding import Binding
from textual.css.query import NoMatches
from textual.events import Key, Resize
from textual.screen import ModalScreen, Screen
from textual.theme import Theme

from pico_tui import commands, palette
from pico_tui import presentation as pres
from pico_tui.command_catalog import ACTIONS_BY_KEY, CommandAction, Parameter, can_command
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
    AcquisitionMode,
    ConnectionMode,
    ConnectionState,
    SensorMode,
    Severity,
    SpectrumSample,
)
from pico_tui.core.state_store import StateStore
from pico_tui.dialogs import (
    ChoiceScreen,
    ConfigRequest,
    ConfigScreen,
    ConfirmScreen,
    ConnectScreen,
    ParameterScreen,
    WirelessNodeScreen,
)
from pico_tui.metrics.experiments import EXPERIMENTS_BY_KEY, ExperimentRecorder
from pico_tui.metrics.export import export_metrics
from pico_tui.metrics.service import MetricsService
from pico_tui.metrics_screens import ExperimentsScreen, MetricsScreen
from pico_tui.dtc_catalog import dtc_description
from pico_tui.preferences import load_preferences, save_preferences
from pico_tui.protocol.router import DecoderRouter
from pico_tui.screens import (
    BaseScreen,
    CommandScreen,
    FftScreen,
    HelpScreen,
    HomeScreen,
    MessagesScreen,
    NetworkScreen,
    NodeScreen,
    SensorScreen,
    WirelessScreen,
)
from pico_tui.security import SecurityManager
from pico_tui.serial_client import SerialClient
from pico_tui.services.controller import DomainController
from pico_tui.services.demo import DemoProducer
from pico_tui.services.log_manager import LogManager
from pico_tui.widgets import MessageEntry

REFRESH_SECONDS = 0.5


class SessionClock:
    """Relógio das medições.

    Em operação normal é o relógio monotônico do sistema. Na reprodução de
    uma gravação ("--replay"), é o instante registrado em cada linha, para
    que os tempos medidos sejam os da sessão gravada, qualquer que seja a
    velocidade de reprodução.
    """

    def __init__(self) -> None:
        self.replay_time: float | None = None

    def __call__(self) -> float:
        return self.replay_time if self.replay_time is not None else time.monotonic()



def _first_replay_instant(lines: list[str]) -> float:
    """Instante, em segundos, da primeira linha datada de uma gravação."""

    for raw in lines:
        if raw.startswith("@"):
            stamp = raw[1:].partition(" ")[0]
            if stamp.isdigit():
                return int(stamp) / 1000.0
    return 0.0


UI_HISTORY_SAMPLES = 120
UI_RECENT_FRAMES = 40
MESSAGE_HISTORY = 2000

# Larguras (em colunas) que mudam a disposição das telas.
BREAKPOINT_MEDIUM = 60
BREAKPOINT_WIDE = 100
BREAKPOINT_SHORT = 30


def build_theme() -> Theme:
    return Theme(
        name="iot-over-can-industrial",
        primary=palette.ACCENT_FOCUS,
        secondary=palette.ACCENT_ACTION,
        accent=palette.ACCENT_FOCUS,
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
    TITLE = "iot-over-can"
    ENABLE_COMMAND_PALETTE = False

    # Cada ação global tem uma tecla de função e uma letra, para terminais sem
    # teclas F (celular, SSH em tablets, consoles seriais).
    BINDINGS = [
        Binding("f1", "show_help", "Ajuda", show=False),
        Binding("question_mark", "show_help", "Ajuda", show=False),
        Binding("f2", "go_home", "Início", show=False),
        Binding("i", "go_home", "Início", show=False),
        Binding("f3", "connection_setup", "Conexão", show=False),
        Binding("p", "connection_setup", "Conexão", show=False),
        Binding("f4", "open_commands", "Comandos", show=False),
        Binding("c", "open_commands", "Comandos", show=False),
        Binding("f5", "show_network", "Rede", show=False),
        Binding("r", "show_network", "Rede", show=False),
        Binding("f6", "show_messages", "Mensagens", show=False),
        Binding("m", "show_messages", "Mensagens", show=False),
        Binding("f7", "show_wireless", "Sensores sem fio", show=False),
        Binding("w", "show_wireless", "Sensores sem fio", show=False),
        # Compatibilidade com a interface anterior. O caminho principal é F7/w,
        # sempre exposto na barra de teclas e na Ajuda.
        Binding("ctrl+a", "show_wireless", "Sensores sem fio", show=False),
        Binding("f8", "show_metrics", "Métricas", show=False),
        Binding("b", "show_metrics", "Métricas", show=False),
        Binding("f9", "show_experiments", "Ensaios", show=False),
        Binding("e", "show_experiments", "Ensaios", show=False),
        Binding("f10", "exit_confirm", "Sair", show=False),
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
        show_messages: bool | None = None,
        ascii_symbols: bool | None = None,
        preferences_path: str | Path | None = None,
        replay: str | Path | None = None,
        replay_speed: float = 1.0,
    ) -> None:
        super().__init__()
        self.replay_path = Path(replay) if replay else None
        self.replay_speed = replay_speed if replay_speed > 0 else 1.0
        self.replaying = False
        self.clock = SessionClock()
        self.initial_port = port
        self.baudrate = baudrate
        self.requested_mode = mode
        self.demo_requested = demo
        self.enable_file_log = enable_file_log
        self.security = SecurityManager(security_mode, security_config)
        self.preferences_path = preferences_path
        self.preferences, self._preferences_warning = load_preferences(preferences_path)
        # Opções de linha de comando valem só para a sessão e não alteram o arquivo.
        self._messages_override = show_messages
        # --ascii vale só para a sessão: não é gravado em tui.json quando outra
        # preferência for salva depois.
        pres.use_ascii_symbols(self.preferences.ascii_symbols if ascii_symbols is None else ascii_symbols)

        self.bus = EventBus()
        self.state_store = StateStore()
        self.controller = DomainController(self.bus, self.state_store)
        self.metrics = MetricsService(self.bus, self.clock)
        self.experiments = ExperimentRecorder(self.bus, self.clock)
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
        self._device_auth_until = 0.0
        self._pending_device_command: str | None = None
        # Comandos mutáveis emitidos enquanto a autenticação do firmware está em
        # curso: são enviados em ordem logo após OK AUTH_UNLOCKED.
        self._pending_device_queue: list[str] = []
        self._device_auth_inflight = False
        self._connecting = False
        self._action_running = False
        self.log_messages: list[str] = []
        self.message_history: deque[MessageEntry] = deque(maxlen=MESSAGE_HISTORY)

        # Compatibilidade com o programa-base e os testes existentes.
        self.connected = False
        self.net_state = "UNKNOWN"
        self.fsm_mode = "—"
        self.telemetry_on = False
        self.simulate_on = False
        self.port_name = ""

    # ------------------------------------------------------------------
    # Ciclo de vida
    # ------------------------------------------------------------------

    def get_default_screen(self) -> Screen:
        return HomeScreen()

    def on_mount(self) -> None:
        self.register_theme(build_theme())
        self.theme = "iot-over-can-industrial"
        self._apply_breakpoints(self.size.width, self.size.height)
        self._register_event_handlers()
        self.run_worker(self._process_lines(), group="decoder", exclusive=True)
        self.set_interval(REFRESH_SECONDS, self._refresh_ui)
        self.set_interval(1.0, self._expire_fragment_transfers)
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
        if self._preferences_warning:
            await self.bus.publish(LogEvent("WARNING", self._preferences_warning, "TUI"))
        if self.security.config_error:
            await self.bus.publish(LogEvent("ERROR", self.security.last_reason, "SECURITY"))
            self.notify(self.security.last_reason, title="Segurança", severity="error", timeout=10)
        elif self.security.mode == "off":
            await self.bus.publish(LogEvent("WARNING", "TUI iniciada com --security-mode off", "SECURITY"))
            self.notify("Modo de segurança OFF ativo durante toda esta sessão.", title="Segurança", severity="warning", timeout=10)
        if self.replay_path is not None:
            await self._start_replay()
            return
        if self.demo_requested:
            await self._start_demo()
            return

        # Política offline-first:
        # a TUI deve sempre abrir, mesmo sem qualquer hardware conectado.
        # Conexão serial é uma ação posterior e explícita do operador.
        self.state_store.set_connection(
            ConnectionState.DISCONNECTED,
            port=self.initial_port or "",
            mode=ConnectionMode.UNKNOWN,
        )
        self.connected = False
        self.port_name = self.initial_port or ""

        if not self.initial_port:
            await self.bus.publish(
                LogEvent(
                    "INFO",
                    "TUI iniciada sem conexão serial. Use F3 (ou p) para conectar.",
                    "SERIAL",
                )
            )
            self.notify(
                "Nenhum equipamento conectado. Pressione F3 ou p para escolher a porta.",
                title="iot-over-can",
                timeout=6,
            )
            return

        ok = await self._connect_serial(self.initial_port, self.requested_mode)
        if not ok:
            await self.bus.publish(
                LogEvent(
                    "WARNING",
                    f"Não foi possível abrir {self.initial_port}. A TUI continuará em modo offline.",
                    "SERIAL",
                )
            )
            self.notify(
                f"Falha ao abrir {self.initial_port}. A TUI continua disponível; escolha outra porta em F3.",
                title="Conexão serial",
                severity="warning",
                timeout=7,
            )


    async def _open_connection_dialog(self, *, exit_on_cancel: bool = False) -> None:
        choice = await self.push_screen_wait(
            ConnectScreen(
                initial_port="" if self._last_port == "DEMO" else self._last_port,
                initial_mode=self._last_mode,
                allow_cancel=not exit_on_cancel,
            )
        )
        if choice is None:
            if exit_on_cancel:
                self.exit()
            return
        if choice.demo:
            await self._start_demo()
            return
        await self._connect_serial(choice.port, choice.mode)

    async def _start_demo(self) -> None:
        self.connected = True
        self.port_name = "DEMO"
        self._last_port = "DEMO"
        self._last_mode = "gateway"
        self.state_store.set_connection(ConnectionState.READY, port="DEMO", mode=ConnectionMode.DEMO)
        await self.bus.publish(LogEvent("INFO", "Modo demonstração iniciado", "DEMO"))
        self.run_worker(self.demo_producer.run(), group="demo", exclusive=True)

    async def _start_replay(self) -> None:
        """Reproduz uma gravação das linhas da Probe 00 ("@<ms> <linha>").

        As gravações em Front/replays/ vêm da bancada virtual
        (Codigo/node-can/test/host/sim_network.cpp). Nada é enviado a
        equipamento algum durante a reprodução.
        """

        path = self.replay_path
        assert path is not None
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except OSError as exc:
            await self.bus.publish(LogEvent("ERROR", f"Não foi possível abrir a gravação {path}: {exc}", "REPLAY"))
            self.notify(f"Gravação não encontrada: {path}", severity="error", timeout=8)
            return
        self.connected = True
        self.replaying = True
        self.port_name = "REPLAY"
        self._last_port = "REPLAY"
        self._last_mode = "gateway"
        self.decoder.requested_mode = "gateway"
        self.decoder.reset()
        # As medições passam a usar o tempo da gravação: o que foi acumulado
        # com o relógio do sistema é descartado antes da primeira linha.
        self.clock.replay_time = _first_replay_instant(lines)
        self.metrics.reset()
        self.experiments.reset()
        self.state_store.set_connection(ConnectionState.READY, port=f"gravação {path.name}", mode=ConnectionMode.GATEWAY_CAN)
        await self.bus.publish(
            LogEvent("INFO", f"Reproduzindo {path.name} a {self.replay_speed:g}x; nenhum comando será enviado", "REPLAY")
        )
        self.run_worker(self._replay_lines(lines), group="replay", exclusive=True)

    async def _replay_lines(self, lines: list[str]) -> None:
        previous_ms: int | None = None
        for raw in lines:
            if not raw.startswith("@"):
                continue
            stamp, _, line = raw[1:].partition(" ")
            try:
                at_ms = int(stamp)
            except ValueError:
                continue
            if previous_ms is not None and at_ms > previous_ms:
                await asyncio.sleep((at_ms - previous_ms) / 1000.0 / self.replay_speed)
            previous_ms = at_ms
            self.clock.replay_time = at_ms / 1000.0
            try:
                await self.decoder.decode(line)
            except Exception as exc:
                self.state_store.increment_network(parse_errors=1)
                await self.bus.publish(LogEvent("ERROR", f"Falha ao decodificar '{line}': {exc}", "PROTOCOL"))
        await self.bus.publish(LogEvent("INFO", "Fim da gravação", "REPLAY"))
        self.notify("Fim da gravação.", timeout=6)

    async def _connect_serial(self, port: str, mode: str) -> bool:
        if self._connecting:
            return False
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
            return False
        # Abrir uma porta pode resetar placas ESP32 via DTR/RTS. Aguarda o boot,
        # descarta bytes residuais e só então inicia o handshake textual.
        await asyncio.sleep(0.85)
        try:
            await asyncio.to_thread(client.discard_input)
        except Exception as exc:
            await self.bus.publish(LogEvent("DEBUG", f"Não foi possível limpar entrada serial: {exc}", "SERIAL"))
        self.serial_client = client
        self.connected = True
        self.port_name = port
        client.set_console_echo_filter(mode.lower() not in {"gateway", "probe"})
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
            "probe": ConnectionMode.GATEWAY_CAN,
            "sensor": ConnectionMode.SENSOR_DIRECT,
            "sensor_direct": ConnectionMode.SENSOR_DIRECT,
        }.get(mode.lower())
        if forced:
            await self.bus.publish(ConnectionModeDetected(forced))
        await self._send_probe(mode)
        self._connecting = False
        return True


    async def _send_probe(self, mode: str) -> None:
        normalized = mode.lower()
        if normalized in {"sensor", "sensor_direct"}:
            self._send_raw("VERSION")
            await asyncio.sleep(0.10)
            for command in ("STATUS", "GET"):
                self._send_raw(command)
                await asyncio.sleep(0.05)
            return

        if normalized in {"gateway", "gateway_can", "probe"}:
            self._send_raw("PROBE_VERSION")
            await asyncio.sleep(0.12)
            self._send_raw("PROBE_STATUS")
            return

        # AUTO: VERSION é deliberadamente o único probe inicial. O Pico responde
        # VERSION; a Probe 00 responde PROBE_VERSION pelo alias compatível.
        self._send_raw("VERSION")
        await asyncio.sleep(0.35)
        if self.decoder.mode == ConnectionMode.GATEWAY_CAN:
            self._send_raw("PROBE_STATUS")
        elif self.decoder.mode == ConnectionMode.SENSOR_DIRECT:
            self._send_raw("STATUS")
            await asyncio.sleep(0.05)
            self._send_raw("GET")
        else:
            # Um único fallback para versões antigas/boot lento, sem rajada de
            # quatro comandos consecutivos.
            self._send_raw("PROBE_VERSION")
            await asyncio.sleep(0.25)
            if self.decoder.mode == ConnectionMode.GATEWAY_CAN:
                self._send_raw("PROBE_STATUS")

    def _close_serial(self) -> None:
        client = self.serial_client
        self.serial_client = None
        self._device_auth_until = 0.0
        self._pending_device_command = None
        self._pending_device_queue.clear()
        self._device_auth_inflight = False
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

    @staticmethod
    def _event_is_main_log_noise(event: LogEvent) -> bool:
        if event.level.upper() != "DEBUG":
            return False
        message = event.message.strip()
        if event.source in {"PROBE", "GATEWAY", "SERIAL"} and (
            message.startswith("GW_UNPARSED:") or message.startswith("AUTO_UNPARSED:")
        ):
            return True
        if event.source == "CAN_MAINT":
            return True
        if event.source == "CAN_NODE" and "LOCAL_SENSOR_DEMO_VALUE=" in message:
            return True
        return False


    async def _on_log_event(self, event: LogEvent) -> None:
        clock = datetime.now().strftime("%H:%M:%S")
        self.log_messages.append(f"{clock} [{event.level}] {event.source}: {event.message}")
        if len(self.log_messages) > 3000:
            del self.log_messages[:1000]
        # O LogManager continua persistindo DEBUG bruto em JSONL. A interface
        # mostra um registro operacional, não um console serial.
        if self._event_is_main_log_noise(event):
            return
        entry = MessageEntry(clock, event.level, event.source, event.message)
        self.message_history.append(entry)
        if entry.level == "DEBUG" and not self.preferences.show_debug_messages:
            return
        for screen in self.screen_stack:
            add = getattr(screen, "add_message", None)
            if add is not None:
                try:
                    add(entry)
                except NoMatches:
                    pass

    async def _on_mode_detected(self, event: ConnectionModeDetected) -> None:
        if self.serial_client:
            self.serial_client.set_console_echo_filter(event.mode == ConnectionMode.SENSOR_DIRECT)
        await self.bus.publish(LogEvent("INFO", f"Modo detectado: {event.mode.value}", "PROTOCOL"))
        # O handshake é serializado exclusivamente por _send_probe(); não envie
        # uma segunda rajada de VERSION/STATUS/GET durante a detecção automática.


    async def _on_command_ack(self, event: CommandAck) -> None:
        command_upper = (event.command or "").upper()
        if "AUTH_UNLOCKED" in command_upper and event.state != "FAILED":
            self._device_auth_until = time.monotonic() + 240.0
            self._device_auth_inflight = False
            pending = self._pending_device_command
            self._pending_device_command = None
            await self.bus.publish(LogEvent("INFO", "Firmware autenticado; enviando comando pendente.", "SECURITY"))
            queued, self._pending_device_queue = self._pending_device_queue, []
            if pending:
                self._send_raw(pending)
            for queued_command in queued:
                self._send_raw(queued_command)
        elif command_upper.startswith("AUTH_") and event.state in {"FAILED", "REJECTED"}:
            self._device_auth_until = 0.0
            self._device_auth_inflight = False
            self._pending_device_command = None
            self._pending_device_queue.clear()

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
        if sensor and event.state in {"FAILED", "REJECTED"} and not command_upper.startswith("AUTH_"):
            reason = str(event.payload.get("REASON", "")) if event.payload else ""
            self.state_store.add_sensor_incident(
                sensor.parent_node_id,
                sensor.child_id,
                Severity.WARNING,
                "COMANDO",
                f"Comando recusado pelo sensor: {reason or event.command}",
                key=f"command:{event.command}",
            )
        node_id = event.payload.get("NODE") if event.payload else None
        if isinstance(node_id, int) and event.state in {"APPLIED", "REJECTED"} and self.state_store.find_node(node_id):
            self.state_store.add_node_incident(
                node_id,
                Severity.INFO if event.state == "APPLIED" else Severity.WARNING,
                "COMANDO",
                f"Comando {'aplicado' if event.state == 'APPLIED' else 'recusado'} pelo módulo: {event.command}",
                key=f"command:{event.command}:{event.state}",
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
                f"Sensor {event.parent_node_id:02d}.{event.child_id:02d}: DTC 0x{event.record.code:04X}, "
                f"{dtc_description(event.record.code)} "
                f"(gravidade {pres.level_label(pres.severity_level(event.record.severity)).lower()})",
                "DTC",
            )
        )
        if event.record.severity.value == "CRITICAL":
            self.notify(
                f"Sensor {event.parent_node_id:02d}.{event.child_id:02d}: {dtc_description(event.record.code)}",
                title=f"DTC crítico 0x{event.record.code:04X}",
                severity="error",
                timeout=8,
            )

    async def _on_spectrum_received(self, event: SpectrumReceived) -> None:
        await self.bus.publish(
            LogEvent("INFO", f"FFT recebida de {event.sample.logical_id}: {len(event.sample.magnitudes)} bins", "FFT")
        )


    def refresh_now(self) -> None:
        self._refresh_ui(force=True)

    def _refresh_ui(self, force: bool = False) -> None:
        self.state_store.refresh_freshness()
        self._poll_experiment()
        state = self.state_store.snapshot(history=UI_HISTORY_SAMPLES, frames=UI_RECENT_FRAMES)
        # Seleção implícita: comandos internos sem alvo (:tel on, :dtc list)
        # usam o primeiro sensor quando nada foi aberto ainda.
        if state.selected_logical_id is None and state.selected_node_id is None:
            first = self.state_store.first_sensor_id()
            if first:
                self.state_store.set_selected(first)
                state.selected_logical_id = first
        selected = None
        if state.selected_logical_id:
            selected = _find_in_state(state, state.selected_logical_id)
        self._sync_compatibility_fields(state, selected)
        screen = self.screen
        if isinstance(screen, BaseScreen):
            try:
                screen.refresh_view(state)
            except NoMatches:
                # A tela ainda está montando; o próximo ciclo atualiza.
                pass

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


    def _send_raw(self, command: str) -> bool:
        if self.replaying:
            self.run_worker(self.bus.publish(LogEvent("INFO", f"> {command} [gravação: não enviado]", "COMMAND")))
            return True
        if self.demo_requested or self.state_store.snapshot().connection_mode == ConnectionMode.DEMO:
            self.run_worker(self.bus.publish(LogEvent("INFO", f"> {command} [DEMO]", "COMMAND")))
            self.demo_producer.handle_command(command)
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

        mode = self.state_store.snapshot().connection_mode
        normalized = " ".join(command.strip().upper().split())
        needs_device_auth = (
            mode == ConnectionMode.SENSOR_DIRECT
            and self.security.command_requires_auth(command)
            and not normalized.startswith("AUTH ")
        )
        if needs_device_auth and time.monotonic() >= self._device_auth_until:
            token = self.security.device_admin_token
            if not token:
                reason = (
                    "Firmware exige token de manutenção para comandos mutáveis. "
                    "Configure device_admin_token em ~/.config/iot-over-can/security.json."
                )
                self.run_worker(self.bus.publish(LogEvent("ERROR", reason, "SECURITY")))
                try:
                    self.notify(reason, title="Segurança", severity="error", timeout=9)
                except Exception:
                    pass
                return False
            if self._device_auth_inflight:
                if len(self._pending_device_queue) >= 32:
                    reason = "Muitos comandos aguardando a autenticação do firmware; comando descartado."
                    self.run_worker(self.bus.publish(LogEvent("WARNING", reason, "SECURITY")))
                    return False
                self._pending_device_queue.append(command)
                self.run_worker(self.bus.publish(LogEvent("DEBUG", "Comando aguardando autenticação do firmware", "SECURITY")))
                return True
            try:
                # O segredo nunca passa pelo EventLog nem pelo histórico visual da TUI.
                # O comando original só é transmitido após receber OK AUTH_UNLOCKED.
                self._pending_device_command = command
                self._device_auth_inflight = True
                client.write_line(f"AUTH UNLOCK {token}")
                self.run_worker(self.bus.publish(LogEvent("DEBUG", "Autenticação de firmware solicitada", "SECURITY")))
                return True
            except (OSError, RuntimeError) as exc:
                self._pending_device_command = None
                self._device_auth_inflight = False
                self.run_worker(self.bus.publish(LogEvent("ERROR", f"Falha ao autenticar firmware: {exc}", "SERIAL")))
                return False

        try:
            client.write_line(command)
        except (OSError, RuntimeError) as exc:
            self.run_worker(self.bus.publish(LogEvent("ERROR", f"Falha de envio: {exc}", "SERIAL")))
            return False
        display_command = command
        if command.strip().upper().startswith("AUTH UNLOCK "):
            display_command = "AUTH UNLOCK <redacted>"
        self.state_store.set_last_action(display_command)
        self.run_worker(self.bus.publish(LogEvent("DEBUG", f"> {display_command}", "COMMAND")))
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
            target = args[0]
            if "." in target and self.state_store.find_sensor(target):
                self.state_store.set_selected(target)
                self.open_target(f"sensor:{target}")
            else:
                try:
                    node_id = int(target, 0)
                except ValueError:
                    node_id = int(target, 16) if all(c in "0123456789abcdefABCDEF" for c in target) else -1
                if self.state_store.find_node(node_id):
                    self.state_store.set_selected_node(node_id)
                    self.open_target(f"node:{node_id:02d}")
                else:
                    await self.bus.publish(LogEvent("WARNING", f"Alvo {target} não encontrado", "COMMAND"))
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
        elif cmd in {"election", "eleicao", "eleição"}:
            await self._send_network_command("Iniciar eleição da rede", "22 00 FF 01")
        elif cmd in {"canstatus", "netstatus"}:
            await self._send_network_command("Solicitar status global", "22 20 FF 00")
        elif cmd == "can":
            await self._command_can(args)
        elif cmd in {"bind", "associar"}:
            await self._command_wireless_bind(args)
        elif cmd in {"unbind", "desassociar"}:
            await self._command_wireless_unbind(args)
        elif cmd in {"wireless", "semfio", "sem-fio"}:
            self.action_show_wireless()
        elif cmd in {"policy", "politica", "política"}:
            await self._command_wireless_policy(args)
        elif cmd in {"metrics", "metricas", "métricas"}:
            if args and args[0].lower() in {"on", "off", "reset"}:
                self._send_raw(commands.metrics(args[0]))
                if args[0].lower() == "reset":
                    self.metrics.reset()
            elif args and args[0].lower() in {"export", "exportar"}:
                self.export_metrics()
            else:
                self.action_show_metrics()
        elif cmd in {"ensaios", "ensaio", "experiments", "fault"}:
            if args and args[0].lower() in {"stop", "parar", "encerrar"}:
                self.stop_experiment()
            elif args and args[0].lower() in EXPERIMENTS_BY_KEY:
                self.start_experiment(args[0].lower())
            else:
                self.action_show_experiments()
        elif cmd == "disconnect":
            await self._disconnect()
        elif cmd == "reconnect":
            await self._reconnect()
        elif cmd == "connect":
            if args:
                mode = args[1] if len(args) > 1 else "auto"
                await self._connect_serial(args[0], mode)
            else:
                await self._open_connection_dialog()
        elif cmd == "export" and args and args[0].lower() == "csv":
            self._export_csv()
        elif cmd == "export" and args and args[0].lower() in {"metrics", "metricas", "métricas"}:
            self.export_metrics()
        elif cmd == "snapshot":
            # Substitui o antigo F12: estado completo em exports/snapshot_*.json.
            self.action_snapshot()
        elif cmd == "save" and args and args[0].lower() == "log":
            await self.bus.publish(LogEvent("INFO", "O log JSONL é persistido continuamente", "LOG"))
        elif cmd in {"home", "inicio", "início"}:
            self.action_go_home()
        elif cmd in {"messages", "mensagens", "log"}:
            self.action_show_messages()
        elif cmd in {"quit", "exit"}:
            self.action_exit_confirm()
        else:
            await self.bus.publish(LogEvent("ERROR", f"Comando interno desconhecido: {cmd}", "COMMAND"))


    async def _send_network_command(self, label: str, command: str) -> None:
        if self.state_store.snapshot(history=0, frames=0).connection_mode == ConnectionMode.DEMO:
            self._send_raw(command)
            self.state_store.set_last_action(f"{label}: {command} (demonstração, nada foi enviado)")
            await self.bus.publish(LogEvent("INFO", f"{label}: {command}", "CAN"))
            return
        if not self.serial_client or not self.serial_client.is_open:
            await self.bus.publish(LogEvent("ERROR", f"Não conectado: {label}", "CAN"))
            self.notify("Conecte-se a uma porta serial (F3) antes de enviar comandos.", severity="warning")
            return
        if self._send_raw(command):
            self.state_store.set_last_action(f"{label}: {command}")
            await self.bus.publish(LogEvent("INFO", f"{label}: {command}", "CAN"))

    async def _command_can(self, args: list[str]) -> None:
        if args:
            command = " ".join(args)
            await self._send_network_command("Comando CAN manual", command)
            return
        state = self.state_store.snapshot(history=0, frames=0)
        node = self.state_store.find_node(state.selected_node_id) if state.selected_node_id is not None else None
        # Sem módulo selecionado, :can abre os comandos da rede inteira.
        if node is None:
            self.open_commands("network")
            return
        self.open_commands(f"node:{node.parent_node_id:02d}")


    def _wireless_observations(self, wireless_uuid: str, *, max_age_s: float = 15.0) -> list[tuple[int, int]]:
        """Retorna observações BLE recentes (módulo, RSSI), melhor sinal primeiro."""

        now = time.monotonic()
        state = self.state_store.snapshot(history=0, frames=0)
        observations: list[tuple[int, int]] = []
        for node_id, node in state.nodes.items():
            candidate = next(
                (item for item in node.wireless_candidates.values() if item.wireless_uuid.lower() == wireless_uuid.lower()),
                None,
            )
            if candidate is None:
                continue
            if candidate.last_seen_monotonic and now - candidate.last_seen_monotonic > max_age_s:
                continue
            observations.append((node_id, candidate.rssi_dbm))
        observations.sort(key=lambda item: item[1], reverse=True)
        return observations

    def _associated_sensor_by_uuid(self, wireless_uuid: str):
        for node in self.state_store.snapshot(history=0, frames=0).nodes.values():
            for sensor in node.sensors.values():
                if (
                    sensor.wireless_uuid
                    and sensor.wireless_uuid.lower() == wireless_uuid.lower()
                    and sensor.association_state.upper() != "UNBOUND"
                ):
                    return sensor
        return None

    def _best_wireless_observation(self, wireless_uuid: str | None = None) -> tuple[str, int] | None:
        state = self.state_store.snapshot(history=0, frames=0)
        associated = {
            sensor.wireless_uuid.lower()
            for node in state.nodes.values()
            for sensor in node.sensors.values()
            if sensor.wireless_uuid and sensor.association_state.upper() != "UNBOUND"
        }
        now = time.monotonic()
        best: tuple[str, int, int] | None = None
        for node_id, node in state.nodes.items():
            for candidate in node.wireless_candidates.values():
                if candidate.wireless_uuid.lower() in associated:
                    continue
                if wireless_uuid and candidate.wireless_uuid.lower() != wireless_uuid.lower():
                    continue
                if candidate.last_seen_monotonic and now - candidate.last_seen_monotonic > 15.0:
                    continue
                if best is None or candidate.rssi_dbm > best[2]:
                    best = (candidate.wireless_uuid, node_id, candidate.rssi_dbm)
        return (best[0], best[1]) if best is not None else None

    async def _command_wireless_bind(self, args: list[str]) -> None:
        uuid_value: str | None = None
        node_id: int | None = None
        if args and args[0].lower() not in {"best", "auto"}:
            uuid_value = args[0]
        if len(args) > 1:
            try:
                node_id = int(args[1], 0)
            except ValueError:
                await self.bus.publish(LogEvent("ERROR", f"Módulo inválido: {args[1]}", "BLE"))
                return
        if uuid_value:
            existing = self._associated_sensor_by_uuid(uuid_value)
            if existing is not None:
                await self.bus.publish(
                    LogEvent(
                        "WARNING",
                        f"{uuid_value} já está associado como {existing.logical_id}; desassocie antes de migrar",
                        "BLE",
                    )
                )
                return
        best = self._best_wireless_observation(uuid_value)
        if best is None:
            await self.bus.publish(LogEvent("WARNING", "Nenhum candidato wireless recente foi observado", "BLE"))
            return
        resolved_uuid, recommended_node = best
        target = node_id if node_id is not None else recommended_node
        option = "AUTO" if len(args) > 2 and args[2].lower() == "auto" else None
        if self._send_raw(commands.wireless_association("BIND", target, resolved_uuid, option)):
            policy = " (reassociação automática)" if option else ""
            await self.bus.publish(
                LogEvent("INFO", f"Associação solicitada: {resolved_uuid} → Módulo {target:02d}{policy}", "BLE")
            )

    async def _command_wireless_policy(self, args: list[str]) -> None:
        """:policy [sensor|uuid] auto|manual"""

        if not args or args[-1].lower() not in {"auto", "manual"}:
            await self.bus.publish(LogEvent("WARNING", "Uso: :policy [sensor|uuid] auto|manual", "BLE"))
            return
        option = args[-1].upper()
        sensor = None
        if len(args) > 1:
            sensor = self.state_store.find_sensor(args[0]) if "." in args[0] else self._associated_sensor_by_uuid(args[0])
        else:
            sensor = self._selected_sensor()
        if sensor is None or not sensor.wireless_uuid or sensor.association_state.upper() == "UNBOUND":
            await self.bus.publish(LogEvent("WARNING", "Selecione um sensor sem fio associado", "BLE"))
            return
        if self._send_raw(commands.wireless_association("POLICY", sensor.parent_node_id, sensor.wireless_uuid, option)):
            await self.bus.publish(
                LogEvent("INFO", f"Política de reassociação de {sensor.logical_id}: {option}", "BLE")
            )

    async def _command_wireless_unbind(self, args: list[str]) -> None:
        if not args:
            sensor = self._selected_sensor()
            if sensor is None or not sensor.wireless_uuid or sensor.association_state.upper() == "UNBOUND":
                await self.bus.publish(
                    LogEvent("WARNING", "Uso: :unbind <uuid> [módulo] ou selecione um sensor associado", "BLE")
                )
                return
            uuid_value = sensor.wireless_uuid
            node_id = sensor.parent_node_id
        else:
            uuid_value = args[0]
            if len(args) > 1:
                try:
                    node_id = int(args[1], 0)
                except ValueError:
                    await self.bus.publish(LogEvent("ERROR", f"Módulo inválido: {args[1]}", "BLE"))
                    return
            else:
                sensor = self._associated_sensor_by_uuid(uuid_value)
                if sensor is None:
                    await self.bus.publish(LogEvent("WARNING", f"Associação não encontrada para {uuid_value}", "BLE"))
                    return
                node_id = sensor.parent_node_id
        if self._send_raw(commands.wireless_association("UNBIND", node_id, uuid_value)):
            await self.bus.publish(LogEvent("INFO", f"Desassociação solicitada: {uuid_value} do Módulo {node_id:02d}", "BLE"))

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
                    "Uso: :acq polling. ACQ DRDY não faz parte do fluxo operacional atual.",
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
            self._send_raw("PROBE_STATUS")

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
            # A conexão direta oferece limpeza global com o comando literal DTC CLEAR.
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


    # ------------------------------------------------------------------
    # Navegação e ações globais
    # ------------------------------------------------------------------

    def global_hints(self, *, home: bool, commands: bool = True, help: bool = True) -> list[tuple[str, str, str | None]]:
        hints: list[tuple[str, str, str | None]] = []
        if not home:
            hints.append(("F2/i", "Início", "app.go_home"))
        if commands:
            hints.append(("F4/c", "Comandos", "app.open_commands"))
        if home:
            hints.extend(
                [
                    ("F3/p", "Conexão", "app.connection_setup"),
                    ("F5/r", "Rede", "app.show_network"),
                    ("F6/m", "Mensagens", "app.show_messages"),
                    ("F7/w", "Sensores sem fio", "app.show_wireless"),
                    ("F8/b", "Métricas", "app.show_metrics"),
                    ("F9/e", "Ensaios", "app.show_experiments"),
                ]
            )
        if help:
            hints.append(("F1/?", "Ajuda", "app.show_help"))
        if home:
            hints.append(("F10/q", "Sair", "app.exit_confirm"))
        return hints

    def security_label(self) -> str:
        label = self.security.status_label()
        return {
            "SEC=OFF": "desligada",
            "SEC=YUBIKEY": "com YubiKey",
            "SEC=LOCKED": "bloqueada, sem YubiKey",
            "SEC=CONFIG_ERROR": "com erro de configuração",
        }.get(label, label.replace("SEC=", "").lower())

    @property
    def messages_on_home(self) -> bool:
        if self._messages_override is not None:
            return self._messages_override
        return self.preferences.show_messages_on_home

    def set_messages_on_home(self, visible: bool) -> None:
        self._messages_override = None
        self.set_preference("show_messages_on_home", visible)
        for screen in self.screen_stack:
            if isinstance(screen, HomeScreen):
                screen.set_messages_visible(visible)

    def set_preference(self, name: str, value: bool) -> None:
        setattr(self.preferences, name, value)
        error = save_preferences(self.preferences, self.preferences_path)
        if error:
            self.notify(error, severity="warning", timeout=6)

    def visible_messages(self) -> list[MessageEntry]:
        show_debug = self.preferences.show_debug_messages
        return [entry for entry in self.message_history if show_debug or entry.level != "DEBUG"]

    def _modal_open(self) -> bool:
        return isinstance(self.screen, ModalScreen)

    def _replace_top(self, screen: Screen) -> None:
        """Abre uma tela de primeiro nível sem empilhar indefinidamente."""

        while len(self.screen_stack) > 1 and not isinstance(self.screen, ModalScreen):
            self.pop_screen()
        self.push_screen(screen)

    def action_go_home(self) -> None:
        if self._modal_open():
            return
        while len(self.screen_stack) > 1 and not isinstance(self.screen, ModalScreen):
            self.pop_screen()

    def action_show_help(self) -> None:
        if not self._modal_open() and not isinstance(self.screen, HelpScreen):
            self.push_screen(HelpScreen())

    def action_show_network(self) -> None:
        if not self._modal_open() and not isinstance(self.screen, NetworkScreen):
            self._replace_top(NetworkScreen())

    def action_show_messages(self) -> None:
        if not self._modal_open() and not isinstance(self.screen, MessagesScreen):
            self._replace_top(MessagesScreen())

    def action_show_wireless(self) -> None:
        if not self._modal_open() and not isinstance(self.screen, WirelessScreen):
            self._replace_top(WirelessScreen())

    def action_show_metrics(self) -> None:
        if not self._modal_open() and not isinstance(self.screen, MetricsScreen):
            self._replace_top(MetricsScreen())

    def action_show_experiments(self) -> None:
        if not self._modal_open() and not isinstance(self.screen, ExperimentsScreen):
            self._replace_top(ExperimentsScreen())

    # ------------------------------------------------------------------
    # Métricas e ensaios
    # ------------------------------------------------------------------

    def export_metrics(self) -> None:
        try:
            paths = export_metrics(self.metrics.snapshot(), list(self.experiments.history))
        except OSError as exc:
            self.notify(f"Falha ao exportar as métricas: {exc}", severity="error", timeout=8)
            return
        self.run_worker(
            self.bus.publish(LogEvent("INFO", "Métricas exportadas: " + ", ".join(str(path) for path in paths), "EXPORT"))
        )
        self.notify(f"Métricas exportadas em {paths[0].parent}", timeout=6)

    def reset_metrics(self) -> None:
        self.metrics.reset()
        mode = self.state_store.snapshot(history=0, frames=0).connection_mode
        if mode == ConnectionMode.GATEWAY_CAN and not self.replaying:
            self._send_raw(commands.metrics("RESET"))
        self.notify("Medições zeradas.", timeout=3)

    def _poll_experiment(self) -> None:
        finished = self.experiments.poll()
        if finished is not None:
            self._experiment_finished(finished)

    def _experiment_finished(self, run) -> None:
        summary = "; ".join(f"{label}: {value}" for label, value in run.rows() if value != "não observado")
        self.run_worker(self.bus.publish(LogEvent("INFO", f"Ensaio concluído: {run.title}. {summary}", "ENSAIO")))
        try:
            self.notify(f"Ensaio concluído: {run.title}", timeout=6)
        except Exception:
            pass

    def stop_experiment(self) -> None:
        run = self.experiments.active
        if run is None:
            self.notify("Nenhum ensaio em curso.", severity="warning", timeout=3)
            return
        if run.experiment.kind != "MANUAL":
            self._send_raw(commands.fault_cancel(run.node_id))
        finished = self.experiments.stop()
        if finished is not None:
            self._experiment_finished(finished)

    def _experiment_node_options(self, target: str) -> tuple[list[tuple[str, str]], str | None]:
        """Módulos elegíveis para um ensaio e o mais indicado para o tipo de alvo."""

        state = self.state_store.snapshot(history=0, frames=0)
        leader = self.experiments.current_leader
        options: list[tuple[str, str]] = []
        preferred: str | None = None
        for node_id in sorted(state.nodes):
            node = state.nodes[node_id]
            if not 1 <= node_id <= 31:
                continue
            notes: list[str] = []
            if node_id == leader or node.role == "LEADER":
                notes.append("líder")
            sensors = [sensor for sensor in node.sensors.values() if sensor.wireless_associated]
            if sensors:
                notes.append(f"{len(sensors)} sensor(es) sem fio")
            label = f"Módulo {node_id:02d}" + (f" · {', '.join(notes)}" if notes else "")
            options.append((label, str(node_id)))
            if target == "leader" and "líder" in notes:
                preferred = str(node_id)
            if target == "owner" and sensors and preferred is None:
                preferred = str(node_id)
        return options, preferred

    @work
    async def start_experiment(self, key: str) -> None:
        experiment = EXPERIMENTS_BY_KEY.get(key)
        if experiment is None:
            return
        if self.experiments.active is not None:
            self.notify("Há um ensaio em curso. Encerre-o com s antes de iniciar outro.", severity="warning", timeout=5)
            return
        mode = self.state_store.snapshot(history=0, frames=0).connection_mode
        if experiment.kind != "MANUAL" and mode not in {ConnectionMode.GATEWAY_CAN, ConnectionMode.DEMO}:
            self.notify("Os ensaios são comandados pela Probe 00. Conecte-se a ela em F3.", severity="warning", timeout=6)
            return

        node_id: int | None = None
        if experiment.target != "none":
            options, preferred = self._experiment_node_options(experiment.target)
            if not options:
                self.notify("Nenhum módulo CAN conhecido nesta sessão.", severity="warning", timeout=5)
                return
            hint = {
                "leader": "O ensaio é aplicado ao líder em exercício; outro módulo pode ser escolhido.",
                "owner": "Para medir a reassociação, escolha um módulo com sensores sem fio.",
            }.get(experiment.target, "Escolha o módulo que sofrerá a falha.")
            chosen = await self.push_screen_wait(ChoiceScreen(experiment.title, hint, options, preferred))
            if chosen is None:
                return
            node_id = int(chosen)

        seconds = experiment.default_seconds
        if experiment.kind not in {"MANUAL", "DROP_SESSION"}:
            action = CommandAction(
                key=f"experiment.{experiment.key}",
                scope="network",
                group="Ensaios",
                title=experiment.title,
                description=experiment.summary,
                parameter=Parameter(
                    name="seconds",
                    label="Duração da falha",
                    default=str(experiment.default_seconds),
                    minimum=experiment.min_seconds,
                    maximum=experiment.max_seconds,
                    unit="s",
                    help="Depois da falha, a rede é observada por mais "
                         f"{experiment.settle_seconds} s antes de o resultado ser fechado.",
                ),
            )
            target_label = f"Módulo {node_id:02d}" if node_id else "barramento CAN"
            value = await self.push_screen_wait(ParameterScreen(action, target_label))
            if value is None:
                return
            seconds = int(value)

        load_percent: int | None = None
        if experiment.kind == "LOAD":
            choice = await self.push_screen_wait(
                ChoiceScreen(
                    experiment.title,
                    "Parcela do tempo de barramento ocupada pela carga artificial.",
                    [(f"{percent} %", str(percent)) for percent in (20, 40, 60, 80)],
                    "40",
                )
            )
            if choice is None:
                return
            load_percent = int(choice)

        command = experiment.command(node_id, seconds, load_percent or 40)
        if command is not None:
            where = f"Módulo {node_id:02d}" if node_id else "Barramento CAN"
            confirmed = await self.push_screen_wait(
                ConfirmScreen(
                    experiment.title,
                    f"{where}\n\n{experiment.effect}\n\nComando: {command}",
                    confirm_label="Iniciar ensaio",
                    danger=True,
                )
            )
            if not confirmed:
                return
            if not self._send_raw(command):
                return
        run = self.experiments.start(experiment, node_id, seconds, load_percent=load_percent)
        await self.bus.publish(LogEvent("WARNING", f"Ensaio iniciado: {run.title}", "ENSAIO"))
        if not isinstance(self.screen, ExperimentsScreen):
            self.action_show_experiments()

    def action_open_commands(self) -> None:
        if self._modal_open() or isinstance(self.screen, CommandScreen):
            return
        screen = self.screen
        if isinstance(screen, SensorScreen):
            self.open_commands(f"sensor:{screen.logical_id}")
        elif isinstance(screen, NodeScreen):
            self.open_commands(f"node:{screen.node_id:02d}")
        elif isinstance(screen, FftScreen):
            self.open_commands(f"sensor:{screen.logical_id}")
        else:
            self.open_commands(None)

    def open_commands(self, target: str | None) -> None:
        if target is None:
            state = self.state_store.snapshot(history=0, frames=0)
            if state.connection_mode == ConnectionMode.SENSOR_DIRECT and state.selected_logical_id:
                target = f"sensor:{state.selected_logical_id}"
            else:
                target = "network"
        self.push_screen(CommandScreen(target))

    def open_target(self, target: str) -> None:
        """Abre a tela própria de um módulo ("node:04") ou sensor ("sensor:04.01")."""

        kind, _, ident = target.partition(":")
        if kind == "sensor" and ident:
            self.state_store.set_selected(ident)
            self.push_screen(SensorScreen(ident))
        elif kind == "node" and ident:
            node_id = int(ident)
            self.state_store.set_selected_node(node_id)
            self.push_screen(NodeScreen(node_id))

    def submit_manual_command(self, text: str) -> None:
        if text.startswith(":"):
            self.run_worker(self._execute_internal_command(text[1:]))
        else:
            self._send_raw(text)

    @work
    async def request_wireless_bind(self, wireless_uuid: str) -> None:
        existing = self._associated_sensor_by_uuid(wireless_uuid)
        if existing is not None:
            self.notify(f"Já associado como {existing.logical_id}.", severity="warning", timeout=4)
            return
        observations = self._wireless_observations(wireless_uuid)
        if not observations:
            self.notify("O candidato não foi observado recentemente por nenhum módulo.", severity="warning", timeout=5)
            return
        state = self.state_store.snapshot(history=0, frames=0)
        profile = "UNKNOWN"
        for node in state.nodes.values():
            candidate = next(
                (item for item in node.wireless_candidates.values() if item.wireless_uuid.lower() == wireless_uuid.lower()),
                None,
            )
            if candidate is not None:
                profile = candidate.profile_id
                break
        node_id = await self.push_screen_wait(WirelessNodeScreen(wireless_uuid, pres.profile_label(profile), observations))
        if node_id is None:
            return
        policy = await self.push_screen_wait(
            ChoiceScreen(
                "Associar sensor sem fio",
                f"Associar {wireless_uuid} ao Módulo {node_id:02d}. O módulo passa a ser responsável pelo "
                "sensor. Com a reassociação automática, o líder entrega o sensor a outro módulo que o alcance "
                "se este sair do ar ou deixar de ouvi-lo.",
                [
                    ("Associar com reassociação automática", "AUTO"),
                    ("Associar; só o operador muda o responsável", "MANUAL"),
                ],
                "AUTO",
            )
        )
        if policy is None:
            return
        option = "AUTO" if policy == "AUTO" else None
        if self._send_raw(commands.wireless_association("BIND", node_id, wireless_uuid, option)):
            detail = " (reassociação automática)" if option else ""
            await self.bus.publish(
                LogEvent("INFO", f"Associação solicitada: {wireless_uuid} → Módulo {node_id:02d}{detail}", "BLE")
            )
            self.notify("Associação solicitada; aguardando confirmação do módulo.", timeout=4)

    @work
    async def request_wireless_policy(self, logical_id: str) -> None:
        """Alterna a política de reassociação de um sensor associado."""

        sensor = self.state_store.find_sensor(logical_id)
        if sensor is None or not sensor.wireless_uuid or sensor.association_state.upper() == "UNBOUND":
            self.notify("O sensor não possui uma associação wireless ativa.", severity="warning", timeout=4)
            return
        current = sensor.failover_policy if sensor.failover_policy in {"AUTO", "MANUAL"} else None
        policy = await self.push_screen_wait(
            ChoiceScreen(
                f"Reassociação do sensor {logical_id}",
                "Define se o líder pode entregar este sensor a outro módulo sem intervenção do operador.",
                [
                    ("Automática: o líder reassocia quando o responsável falha", "AUTO"),
                    ("Manual: só o operador muda o responsável", "MANUAL"),
                ],
                current,
            )
        )
        if policy is None or policy == current:
            return
        if self._send_raw(commands.wireless_association("POLICY", sensor.parent_node_id, sensor.wireless_uuid, policy)):
            await self.bus.publish(LogEvent("INFO", f"Política de reassociação de {logical_id}: {policy}", "BLE"))
            self.notify("Política enviada; aguardando confirmação do módulo.", timeout=4)

    @work
    async def request_wireless_unbind(self, logical_id: str) -> None:
        sensor = self.state_store.find_sensor(logical_id)
        if sensor is None or not sensor.wireless_uuid or sensor.association_state.upper() == "UNBOUND":
            self.notify("O sensor não possui uma associação wireless ativa.", severity="warning", timeout=4)
            return
        confirmed = await self.push_screen_wait(
            ConfirmScreen(
                "Desassociar sensor sem fio",
                f"Desassociar {logical_id} ({sensor.wireless_uuid}) do Módulo {sensor.parent_node_id:02d}?\n\n"
                "O sensor voltará a aparecer como disponível se continuar anunciando por BLE. "
                "O item lógico só será removido depois da confirmação do módulo.",
                confirm_label="Desassociar",
                danger=True,
            )
        )
        if not confirmed:
            return
        if self._send_raw(commands.wireless_association("UNBIND", sensor.parent_node_id, sensor.wireless_uuid)):
            await self.bus.publish(
                LogEvent("INFO", f"Desassociação solicitada: {sensor.wireless_uuid} do Módulo {sensor.parent_node_id:02d}", "BLE")
            )
            self.notify("Desassociação solicitada; aguardando UNBOUND do módulo.", timeout=4)

    def start_action(self, action_key: str, target: str, value: str | None = None) -> None:
        """Executa uma ação do catálogo, pedindo parâmetro e confirmação se preciso."""

        if self._action_running:
            self.notify("Aguarde a ação anterior terminar.", severity="warning", timeout=3)
            return
        self.run_worker(self._action_flow(action_key, target, value), group="command-flow")

    async def _action_flow(self, action_key: str, target: str, value: str | None) -> None:
        action = ACTIONS_BY_KEY.get(action_key)
        if action is None:
            return
        self._action_running = True
        try:
            target_label = _target_label(target)
            if action.parameter is not None and value is None:
                value = await self.push_screen_wait(ParameterScreen(action, target_label))
                if value is None:
                    return
            if action.confirm:
                confirmed = await self.push_screen_wait(
                    ConfirmScreen(action.title, f"{target_label}\n\n{action.confirm}", confirm_label="Enviar", danger=True)
                )
                if not confirmed:
                    return
            await self.execute_action(action_key, target, value)
        finally:
            self._action_running = False

    async def execute_action(self, action_key: str, target: str, value: str | None = None) -> None:
        """Traduz uma ação do catálogo nos comandos de protocolo existentes."""

        action = ACTIONS_BY_KEY[action_key]
        kind, _, ident = target.partition(":")
        if action.scope == "network":
            if action_key == "network.probe_status":
                self._send_raw("PROBE_STATUS")
                self.state_store.set_last_action("Probe 00: PROBE_STATUS")
                return
            await self._send_network_command(action.title, can_command(action_key, None, value))
            return
        if action.scope == "node":
            node_id = int(ident)
            self.state_store.set_selected_node(node_id)
            await self._send_network_command(f"{action.title} (módulo {node_id:02d})", can_command(action_key, node_id, value))
            return

        # Telemetria e configuração de um filho wireless só existem enquanto o
        # Node responsável mantém uma sessão autenticada com o sensor. Sem ela
        # o comando não é enviado: o Node o recusaria e a tela sugeriria um
        # canal que não está disponível.
        sensor = self.state_store.find_sensor(ident)
        mode = self.state_store.snapshot(history=0, frames=0).connection_mode
        if (
            sensor is not None
            and sensor.wireless_associated
            and not sensor.has_data_plane
            and mode == ConnectionMode.GATEWAY_CAN
        ):
            reason = pres.data_link_label(sensor.data_link_state)
            self.notify(
                f"Este sensor está associado, mas o plano de dados não está ativo ({reason}). "
                "Use F7/w para gerenciar o vínculo.",
                severity="warning",
                timeout=7,
            )
            await self.bus.publish(
                LogEvent("WARNING", f"Ação {action.title} não enviada a {ident}: plano de dados wireless indisponível", "BLE")
            )
            return
        self.state_store.set_selected(ident)
        self.state_store.set_last_action(f"{action.title} (sensor {ident})")
        if action_key == "sensor.status":
            await self._request_status()
        elif action_key == "sensor.telemetry_on":
            await self._set_telemetry(True)
        elif action_key == "sensor.telemetry_off":
            await self._set_telemetry(False)
        elif action_key == "sensor.telemetry_once":
            await self._execute_telemetry_control("ONCE")
        elif action_key == "sensor.telemetry_period":
            await self._execute_telemetry_control("PERIOD", int(value or 1000))
        elif action_key == "sensor.fft":
            requested_at = time.time()
            if await self._request_fft_for_selected(int(value or 64), "VIEW_ONLY"):
                if not (isinstance(self.screen, FftScreen) and self.screen.logical_id == ident):
                    self.push_screen(FftScreen(ident, requested_at=requested_at))
                else:
                    self.screen.requested_at = requested_at
        elif action_key == "sensor.configure":
            sensor = self.state_store.find_sensor(ident)
            if sensor is not None:
                request = await self.push_screen_wait(ConfigScreen(sensor))
                if request:
                    await self._apply_config_request(ident, request)
        elif action_key == "sensor.stop_now":
            await self._stop_telemetry_stream()
        elif action_key == "sensor.restart_acq":
            await self._restart_acquisition()
        elif action_key == "sensor.dtc_refresh":
            await self._send_dtc_list()
        elif action_key == "sensor.dtc_clear":
            await self._send_dtc_clear(None)
        elif action_key == "sensor.wifi_status":
            await self._wifi_status()
        elif action_key == "sensor.wifi_on":
            await self._set_wifi(True)
        elif action_key == "sensor.wifi_off":
            await self._set_wifi(False)

    @work
    async def action_connection_setup(self) -> None:
        if self._modal_open():
            return
        await self._open_connection_dialog()

    @work
    async def action_exit_confirm(self) -> None:
        if self._modal_open():
            return
        confirmed = await self.push_screen_wait(
            ConfirmScreen("Sair", "Encerrar a TUI? A conexão serial será fechada.", confirm_label="Sair")
        )
        if confirmed:
            self.exit()

    def action_stop_telemetry(self) -> None:
        self.run_worker(self._stop_telemetry_stream())

    def action_snapshot(self) -> None:
        output = self._snapshot()
        self.run_worker(self.bus.publish(LogEvent("INFO", f"Snapshot salvo: {output}", "EXPORT")))

    def on_key(self, event: Key) -> None:
        # Textual 1.x reserva Ctrl+C antes do sistema de bindings. O tratamento
        # explícito mantém o mesmo comportamento nas versões 1.x e 8.x.
        if event.key == "ctrl+c":
            event.prevent_default()
            event.stop()
            self.action_stop_telemetry()

    def on_resize(self, event: Resize) -> None:
        self._apply_breakpoints(event.size.width, event.size.height)

    def _apply_breakpoints(self, width: int, height: int) -> None:
        self.set_class(width < BREAKPOINT_MEDIUM, "bp-narrow")
        self.set_class(BREAKPOINT_MEDIUM <= width < BREAKPOINT_WIDE, "bp-medium")
        self.set_class(width >= BREAKPOINT_WIDE, "bp-wide")
        self.set_class(height < BREAKPOINT_SHORT, "bp-short")


def _target_label(target: str) -> str:
    kind, _, ident = target.partition(":")
    if kind == "sensor":
        return f"Alvo: sensor {ident}"
    if kind == "node":
        return f"Alvo: módulo {ident}"
    return "Alvo: rede CAN inteira"


def _find_in_state(state: Any, logical_id: str) -> Any:
    try:
        parent, child = (int(part) for part in logical_id.split(".", 1))
    except ValueError:
        return None
    node = state.nodes.get(parent)
    return node.sensors.get(child) if node else None


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
