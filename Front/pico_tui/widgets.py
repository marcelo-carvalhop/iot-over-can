from __future__ import annotations

import time
from collections import deque
from typing import Any

from textual import on
from textual.app import ComposeResult
from textual.containers import Horizontal, Vertical
from textual.message import Message
from textual.widgets import Button, DataTable, Label, OptionList, RichLog, Sparkline, Static, Tree
from textual.widgets.option_list import Option

from pico_tui import commands, palette
from pico_tui.core.models import AppState, PhysicalNode, SensorNode


class ModeSelected(Message):
    def __init__(self, mode: str) -> None:
        self.mode = mode
        super().__init__()


class WindowSelected(Message):
    def __init__(self, window: str) -> None:
        self.window = window
        super().__init__()


class QuickApplyRequested(Message):
    """Solicita a aplicação explícita da configuração rápida estagiada."""


class NetworkTreePanel(Vertical):
    """Árvore funcional: módulos CAN → sensores wireless. Probe 00 fica fora da árvore."""

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(id="network-panel", classes="panel", **kwargs)
        self._fingerprint: tuple[Any, ...] | None = None

    def compose(self) -> ComposeResult:
        yield Label("REDE E SENSORES", classes="panel-title")
        yield Static("Nenhum dispositivo conectado", id="network-summary")
        yield Tree("Sistema", id="node-tree")

    def refresh_state(self, state: AppState) -> None:
        summary = self.query_one("#network-summary", Static)
        sensor_count = sum(len(node.sensors) for node in state.nodes.values())
        summary.update(
            f"{state.connection_mode.value}  •  nós={len(state.nodes)}  •  sensores={sensor_count}"
        )
        fingerprint = tuple(
            (
                parent,
                node.node_type,
                node.status.value,
                node.can_state,
                node.wifi_state,
                tuple((child, sensor.wireless_uuid, sensor.status.value) for child, sensor in sorted(node.sensors.items())),
            )
            for parent, node in sorted(state.nodes.items())
        )
        if fingerprint == self._fingerprint:
            self._refresh_labels(state)
            return
        self._fingerprint = fingerprint
        tree = self.query_one("#node-tree", Tree)
        tree.root.remove_children()
        tree.root.set_label("Rede CAN FD / Bancada")
        for parent_id, node in sorted(state.nodes.items()):
            marker = palette.STATUS_MARKERS.get(node.status.value, "[N/A]")
            selected = "▶" if state.selected_node_id == parent_id and not state.selected_logical_id else " "
            local = f" local={node.local_sensor_profile}" if node.local_sensor_profile not in {"", "NONE"} else ""
            label = f"{selected} {marker} Node {parent_id:02d} — {node.role} — CAN={node.can_state}{local} — BLE={node.wireless_discovery_state} cand={node.wireless_candidate_count}"
            parent = tree.root.add(label, data=("node", parent_id), expand=True)
            for child_id, sensor in sorted(node.sensors.items()):
                quality = palette.QUALITY_MARKERS.get(sensor.quality.value, "[N/A]")
                uuid = f" {sensor.wireless_uuid}" if sensor.wireless_uuid else ""
                selected = "▶" if state.selected_logical_id == sensor.logical_id else " "
                parent.add(
                    f"{selected} {quality} {sensor.logical_id} {sensor.profile_id or 'UNKNOWN'}{uuid}",
                    data=("sensor", parent_id, child_id),
                    allow_expand=False,
                )
        tree.root.expand()

    def _refresh_labels(self, state: AppState) -> None:
        tree = self.query_one("#node-tree", Tree)
        for node in tree.root.children:
            data = node.data
            if not isinstance(data, tuple) or not data:
                continue
            if data[0] == "node":
                physical = state.nodes.get(data[1])
                if physical:
                    marker = palette.STATUS_MARKERS.get(physical.status.value, "[N/A]")
                    selected = "▶" if state.selected_node_id == physical.parent_node_id and not state.selected_logical_id else " "
                    local = f" local={physical.local_sensor_profile}" if physical.local_sensor_profile not in {"", "NONE"} else ""
                    node.set_label(f"{selected} {marker} Node {physical.parent_node_id:02d} — {physical.role} — CAN={physical.can_state}{local} — BLE={physical.wireless_discovery_state} cand={physical.wireless_candidate_count}")
                for child_node in node.children:
                    child_data = child_node.data
                    if isinstance(child_data, tuple) and child_data[0] == "sensor":
                        sensor = state.nodes.get(child_data[1], None)
                        sensor = sensor.sensors.get(child_data[2]) if sensor else None
                        if sensor:
                            quality = palette.QUALITY_MARKERS.get(sensor.quality.value, "[N/A]")
                            uuid = f" {sensor.wireless_uuid}" if sensor.wireless_uuid else ""
                            selected = "▶" if state.selected_logical_id == sensor.logical_id else " "
                            child_node.set_label(f"{selected} {quality} {sensor.logical_id} {sensor.profile_id or 'UNKNOWN'}{uuid}")



class CanNetworkDashboardPanel(Vertical):
    """Tela principal genérica da rede CAN/CAN FD."""

    ROWS: list[tuple[str, str]] = [
        ("connection", "Conexão"),
        ("gateway", "Instrumentação / Probe 00"),
        ("can", "Barramento CAN"),
        ("network", "Tráfego"),
        ("modules", "Módulos CAN"),
        ("wifi_sensors", "Sensores Wi-Fi"),
        ("selected", "Alvo selecionado"),
        ("selected_status", "Status do alvo"),
        ("last_action", "Última ação"),
    ]

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(id="can-dashboard-panel", classes="panel", **kwargs)

    def compose(self) -> ComposeResult:
        yield Label("CONSOLE DA REDE CAN", classes="panel-title")
        yield Static(
            "Tela principal genérica. Selecione um módulo CAN na árvore para comandos do nó; "
            "selecione um sensor lógico para abrir a telemetria especializada.",
            id="can-dashboard-hint",
        )
        yield DataTable(id="can-dashboard-table", cursor_type="row", zebra_stripes=False)

    def on_mount(self) -> None:
        table = self.query_one("#can-dashboard-table", DataTable)
        table.add_column("Campo", key="field", width=24)
        table.add_column("Valor", key="value", width=52)
        for key, label in self.ROWS:
            table.add_row(label, "N/A", key=key)

    def refresh_state(self, state: AppState, selected_node: PhysicalNode | None, selected_sensor: SensorNode | None) -> None:
        sensor_count = sum(len(node.sensors) for node in state.nodes.values())
        candidate_observations = sum(len(node.wireless_candidates) for node in state.nodes.values())
        unique_candidate_uuids = {
            candidate.wireless_uuid
            for node in state.nodes.values()
            for candidate in node.wireless_candidates.values()
        }
        online_nodes = sum(1 for node in state.nodes.values() if node.status.value == "ONLINE")
        stale_nodes = sum(1 for node in state.nodes.values() if node.status.value in {"AGING", "STALE", "LOST"})
        if selected_sensor:
            selected = f"Sensor {selected_sensor.logical_id} no Node {selected_sensor.parent_node_id:02d}"
            selected_status = (
                f"{selected_sensor.status.value}; modo={selected_sensor.sensor_mode.value}; "
                f"aquisição={selected_sensor.acquisition_mode.value}; DTC={selected_sensor.active_dtc_count}"
            )
        elif selected_node:
            selected = f"Node {selected_node.parent_node_id:02d} — {selected_node.node_type}"
            local_value = (
                f"0x{selected_node.local_sensor_value:02X}"
                if selected_node.local_sensor_value is not None
                else "N/A"
            )
            selected_status = (
                f"status={selected_node.status.value}; role={selected_node.role}; CAN={selected_node.can_state}; "
                f"local={selected_node.local_sensor_profile}:{local_value}; "
                f"wireless={selected_node.wireless_ap_state}; filhos={len(selected_node.sensors)}"
            )
        else:
            selected = "Rede CAN completa"
            selected_status = "Nenhum alvo específico selecionado"

        values = {
            "connection": f"{state.connection_state.value} / {state.connection_mode.value} / porta={state.port or 'N/A'}",
            "gateway": f"Probe 00; CAN={state.gateway.can_state}; serial={state.gateway.serial_port or state.port or 'N/A'}",
            "can": (
                f"arb={state.gateway.arbitration_bitrate or 'N/A'}; "
                f"data={state.gateway.data_bitrate or 'N/A'}; "
                f"bus_off={state.network.bus_off}; passive={state.network.error_passive}"
            ),
            "network": (
                f"RX={state.network.frames_rx}; TX={state.network.frames_tx}; "
                f"CRC={state.network.crc_errors}; parse={state.network.parse_errors}; "
                f"transferências={state.network.active_transfers}"
            ),
            "modules": f"{len(state.nodes)} módulos conhecidos; online={online_nodes}; atenção={stale_nodes}",
            "wifi_sensors": (f"candidatos únicos={len(unique_candidate_uuids)}; "
                             f"observações={candidate_observations}; associados={sensor_count}"),
            "selected": selected,
            "selected_status": selected_status,
            "last_action": state.last_action or "N/A",
        }
        table = self.query_one("#can-dashboard-table", DataTable)
        for key, _ in self.ROWS:
            table.update_cell(key, "value", values.get(key, "N/A"))
        if selected_sensor:
            self.query_one("#can-dashboard-hint", Static).update(
                "Sensor selecionado: a telemetria especializada abaixo mostra métricas de vibração."
            )
        elif selected_node:
            self.query_one("#can-dashboard-hint", Static).update(
                "Módulo CAN selecionado: use F4 ou :can para enviar configurações/comandos ao nó."
            )
        else:
            self.query_one("#can-dashboard-hint", Static).update(
                "Tela principal genérica da rede CAN. Selecione um Node ou sensor na árvore."
            )


class TelemetryPanel(Vertical):
    ROWS: list[tuple[str, str]] = [
        ("mode", "Modo FSM"),
        ("acquisition", "Aquisição"),
        ("axis", "Eixo"),
        ("fft_valid", "FFT resumida"),
        ("rms", "RMS"),
        ("kurtosis", "Curtose (excesso)"),
        ("crest", "Fator de Crista"),
        ("peak_hz", "Freq. de Pico"),
        ("peak_amp", "Amplitude de Pico"),
        ("entropy", "Entropia Espectral"),
        ("ppv", "PPV"),
        ("stalta", "STA/LTA"),
        ("clip", "Clipping"),
        ("battery", "Bateria"),
        ("quality", "Qualidade"),
        ("sequence", "Sequência / perda"),
        ("updated", "Última atualização"),
    ]

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(id="telemetry-panel", classes="panel", **kwargs)
        self._rms_history: deque[float] = deque(maxlen=90)
        self._last_sensor_id: str | None = None

    def compose(self) -> ComposeResult:
        yield Label("SENSOR DE VIBRAÇÃO — DETALHE", classes="panel-title")
        yield Static("Selecione um sensor", id="telemetry-target")
        yield DataTable(id="telemetry-table", cursor_type="row", zebra_stripes=False)
        yield Label("Tendência RMS", classes="config-label")
        yield Sparkline([], id="rms-sparkline")

    def on_mount(self) -> None:
        table = self.query_one("#telemetry-table", DataTable)
        table.add_column("Métrica", key="metric", width=24)
        table.add_column("Valor", key="value", width=22)
        for key, label in self.ROWS:
            table.add_row(label, "N/A", key=key)

    def show_sensor(self, sensor: SensorNode | None) -> None:
        if sensor is None:
            self.clear()
            return
        self.query_one("#telemetry-target", Static).update(
            f"[b]{sensor.logical_id}[/b]  UUID={sensor.wireless_uuid or 'N/A'}"
        )
        if self._last_sensor_id != sensor.logical_id:
            self._last_sensor_id = sensor.logical_id
            self._rms_history.clear()
        sample = sensor.latest_telemetry
        if sample is None:
            self._set_values({"mode": sensor.sensor_mode.value, "acquisition": sensor.acquisition_mode.value})
            return
        if sample.rms is not None:
            self._rms_history.append(sample.rms)
            self.query_one("#rms-sparkline", Sparkline).data = list(self._rms_history)
        age = max(0.0, time.time() - sample.received_wall_time)
        battery = "N/A"
        if sample.battery.valid:
            parts: list[str] = []
            if sample.battery.percentage is not None:
                parts.append(f"{sample.battery.percentage:.0f}%")
            if sample.battery.voltage_v is not None:
                parts.append(f"{sample.battery.voltage_v:.3f} V")
            battery = " / ".join(parts) or "N/A"
        values = {
            "mode": sample.mode.value,
            "acquisition": sample.acquisition_mode.value,
            "axis": sample.axis or "N/A",
            "fft_valid": _fft_valid_text(sample.fft_valid),
            "rms": _fmt(sample.rms, 5, sample.rms_unit),
            "kurtosis": _fmt(sample.kurtosis, 4),
            "crest": _fmt(sample.crest_factor, 4),
            "peak_hz": _fft_metric(sample.peak_frequency_hz, sample.fft_valid, 3, "Hz"),
            "peak_amp": _fft_metric(sample.peak_amplitude, sample.fft_valid, 6),
            "entropy": _fft_metric(sample.spectral_entropy, sample.fft_valid, 4),
            "ppv": _fmt(sample.ppv_mm_s, 4, "mm/s"),
            "stalta": _bool_text(sample.stalta_triggered),
            "clip": "SATURADO" if sample.clipping is True else ("NÃO" if sample.clipping is False else "N/A"),
            "battery": battery,
            "quality": palette.QUALITY_MARKERS.get(sample.quality.value, "[N/A]") + " " + sample.quality.value,
            "sequence": (
                f"{sample.sequence if sample.sequence is not None else 'N/A'} / {sensor.loss_percent:.2f}%"
            ),
            "updated": f"há {age:.1f} s",
        }
        self._set_values(values)

    def _set_values(self, values: dict[str, object]) -> None:
        table = self.query_one("#telemetry-table", DataTable)
        for key, _ in self.ROWS:
            table.update_cell(key, "value", str(values.get(key, "N/A")))

    def clear(self) -> None:
        self.query_one("#telemetry-target", Static).update("Selecione um sensor")
        self._set_values({})
        self._rms_history.clear()
        self.query_one("#rms-sparkline", Sparkline).data = []


class NodeTelemetryPanel(Vertical):
    """Estado contínuo dos módulos CAN; evita transformar telemetria em EventLog."""

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(id="node-telemetry-panel", classes="panel", **kwargs)

    def compose(self) -> ComposeResult:
        yield Label("TELEMETRIA DOS NÓS", classes="panel-title")
        yield Static(
            "Valores contínuos e qualidade do enlace BLE. Eventos permanecem no painel central.",
            id="node-telemetry-hint",
        )
        yield Static("Aguardando módulos CAN...", id="node-telemetry-live")

    def refresh_state(self, state: AppState) -> None:
        now = time.monotonic()
        if not state.nodes:
            text = "Aguardando módulos CAN..."
        else:
            blocks: list[str] = []
            for node_id, node in sorted(state.nodes.items()):
                selected = "▶" if state.selected_node_id == node_id and not state.selected_logical_id else " "
                value = f"0x{node.local_sensor_value:02X}" if node.local_sensor_value is not None else "N/A"
                round_text = str(node.local_sensor_last_round) if node.local_sensor_last_round is not None else "N/A"
                if node.local_sensor_last_seen_monotonic:
                    local_age = max(0.0, now - node.local_sensor_last_seen_monotonic)
                    age_text = f"{local_age:.1f}s"
                else:
                    age_text = "N/A"
                candidates = list(node.wireless_candidates.values())
                best_rssi = max((candidate.rssi_dbm for candidate in candidates), default=None)
                rssi_text = f"{best_rssi} dBm" if best_rssi is not None else "N/A"
                blocks.append(
                    f"[b]{selected} Node {node_id:02d}[/b]  {node.role}\n"
                    f"  local {node.local_sensor_profile or 'NONE'}  valor={value}  rodada={round_text}  há={age_text}\n"
                    f"  BLE={node.wireless_discovery_state}  cand={len(candidates)}  melhor={rssi_text}"
                )
            text = "\n\n".join(blocks)
        self.query_one("#node-telemetry-live", Static).update(text)


class ConfigPanel(Vertical):
    """Configuração rápida; a janela F4 oferece a edição completa."""

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(id="config-panel", classes="panel", **kwargs)
        self.current_mode = "N/A"
        self.current_window = "N/A"
        self.current_rate = "N/A"
        self.current_stalta = "N/A"
        self.current_gain = "N/A"
        self.current_drdy = "N/A"
        self.current_transaction = "IDLE"
        self.target = "N/A"

    def compose(self) -> ComposeResult:
        yield Label("CONFIGURAÇÃO RÁPIDA", classes="panel-title")
        yield Static("Alvo: N/A", id="config-target")
        yield Label("Modo FSM", classes="config-label")
        yield OptionList(*[Option(name, id=name) for name in commands.DEFAULT_FSM_MODES], id="mode-list")
        yield Label("Janela espectral", classes="config-label")
        yield OptionList(*[Option(name, id=name) for name in commands.DEFAULT_WINDOW_TYPES], id="window-list")
        yield Static(id="config-current")
        with Horizontal(id="quick-config-actions"):
            yield Button("Aplicar", id="quick-apply", variant="primary", disabled=True)
        yield Static("Enter: estagiar campo  •  Aplicar: confirmar no firmware", id="config-hint")

    def on_mount(self) -> None:
        self._refresh()

    def show_sensor(self, sensor: SensorNode | None) -> None:
        if sensor is None:
            self.target = "N/A"
            self.current_mode = self.current_window = self.current_rate = "N/A"
            self.current_stalta = self.current_gain = self.current_drdy = "N/A"
            self.current_transaction = "IDLE"
            try:
                self.query_one("#quick-apply", Button).disabled = True
            except Exception:
                pass
            self._refresh()
            return
        cfg = sensor.configuration
        health = sensor.health
        self.target = sensor.logical_id
        self.current_mode = cfg.mode.value
        self.current_window = cfg.window_type or "N/A"
        if cfg.sample_rate_effective_hz is not None and cfg.sample_rate_requested_hz is not None:
            self.current_rate = f"{cfg.sample_rate_requested_hz:g} → {cfg.sample_rate_effective_hz:g} Hz"
        elif cfg.sample_rate_effective_hz is not None:
            self.current_rate = f"{cfg.sample_rate_effective_hz:g} Hz"
        else:
            self.current_rate = "N/A"
        self.current_stalta = _fmt(cfg.stalta_threshold, 3)
        self.current_gain = _fmt(cfg.calibration_gain, 3)
        self.current_transaction = cfg.transaction_state or "IDLE"
        try:
            self.query_one("#quick-apply", Button).disabled = False
        except Exception:
            pass
        if sensor.acquisition_mode.value == "POLLING":
            self.current_drdy = "aquisição=POLLING (OK) • DRDY desativado"
        elif sensor.acquisition_mode.value == "DRDY":
            irq = health.drdy_irq_count if health.drdy_irq_count is not None else "N/A"
            missed = health.drdy_missed_count if health.drdy_missed_count is not None else "N/A"
            self.current_drdy = f"DRDY experimental: IRQ={irq} perdidas={missed}"
        else:
            self.current_drdy = f"aquisição={sensor.acquisition_mode.value}"
        self._refresh()

    def _refresh(self) -> None:
        try:
            self.query_one("#config-target", Static).update(f"Alvo: [b]{self.target}[/b]")
            self.query_one("#config-current", Static).update(
                f"[b]Aplicado[/b]\n"
                f"modo={self.current_mode}\njanela={self.current_window}\n"
                f"taxa={self.current_rate}\nSTA/LTA={self.current_stalta}\n"
                f"ganho={self.current_gain}\n{self.current_drdy}\n"
                f"transação={self.current_transaction}"
            )
        except Exception:
            pass

    @on(OptionList.OptionSelected, "#mode-list")
    def _mode_selected(self, event: OptionList.OptionSelected) -> None:
        if event.option.id:
            self.post_message(ModeSelected(event.option.id))

    @on(OptionList.OptionSelected, "#window-list")
    def _window_selected(self, event: OptionList.OptionSelected) -> None:
        if event.option.id:
            self.post_message(WindowSelected(event.option.id))

    @on(Button.Pressed, "#quick-apply")
    def _apply_requested(self) -> None:
        self.post_message(QuickApplyRequested())


class QuickStatusPanel(Vertical):
    def __init__(self, **kwargs: Any) -> None:
        super().__init__(id="quick-panel", classes="panel", **kwargs)

    def compose(self) -> ComposeResult:
        yield Label("ESTADO RÁPIDO", classes="panel-title")
        yield Static("Sem dados", id="quick-status")

    def refresh_state(self, state: AppState, sensor: SensorNode | None) -> None:
        gateway = state.gateway
        network = state.network
        sensor_count = sum(len(node.sensors) for node in state.nodes.values())
        if sensor is None:
            sensor_text = f"Alvo: rede/módulo CAN\nMódulos: {len(state.nodes)}\nSensores Wi-Fi: {sensor_count}"
        else:
            sev = sensor.highest_severity.value if sensor.highest_severity else "NONE"
            batt = "N/A"
            if sensor.latest_telemetry and sensor.latest_telemetry.battery.valid:
                value = sensor.latest_telemetry.battery.percentage
                batt = f"{value:.0f}%" if value is not None else "instrumentada"
            if sensor.acquisition_mode.value == "POLLING":
                acq_text = "Aquisição: POLLING\nEstado aquisição: OK\nDRDY: desativado"
            elif sensor.acquisition_mode.value == "DRDY":
                acq_text = (
                    f"Aquisição: DRDY (experimental)\nDRDY IRQ: {sensor.health.drdy_irq_count or 0} / "
                    f"perdidas: {sensor.health.drdy_missed_count or 0}"
                )
            else:
                acq_text = f"Aquisição: {sensor.acquisition_mode.value}"
            dtc_count = sensor.reported_dtc_count if sensor.reported_dtc_count is not None else sensor.active_dtc_count
            sensor_text = (
                f"Sensor: [b]{sensor.logical_id}[/b]\n"
                f"Estado: {sensor.status.value}\n{acq_text}\n"
                f"DTC: {dtc_count} / {sev}\nBateria: {batt}\n"
                f"Perda: {sensor.loss_percent:.2f}%"
            )
        self.query_one("#quick-status", Static).update(
            f"Probe 00: CAN={gateway.can_state} serial={gateway.serial_port or state.port or 'N/A'}\n"
            f"CAN FD: {gateway.arbitration_bitrate or 'N/A'} / {gateway.data_bitrate or 'N/A'}\n"
            f"RX/TX: {network.frames_rx}/{network.frames_tx}\n"
            f"CRC: {network.crc_errors}  Transf.: {network.active_transfers}\n\n"
            f"{sensor_text}"
        )


class EventLog(Vertical):
    def __init__(self, **kwargs: Any) -> None:
        super().__init__(id="event-log-container", classes="panel", **kwargs)

    def compose(self) -> ComposeResult:
        yield Label("EVENTOS E COMANDOS", classes="panel-title")
        yield RichLog(id="event-log", markup=True, wrap=True, highlight=False, max_lines=1000)

    def write(self, markup_line: str) -> None:
        self.query_one("#event-log", RichLog).write(markup_line)


def _fmt(value: object, decimals: int = 3, unit: str = "") -> str:
    if value is None:
        return "N/A"
    try:
        text = f"{float(value):.{decimals}f}"
    except (TypeError, ValueError):
        text = str(value)
    return f"{text} {unit}".strip()


def _bool_text(value: bool | None) -> str:
    if value is None:
        return "N/A"
    return "SIM" if value else "NÃO"


def _fft_valid_text(value: bool | None) -> str:
    if value is True:
        return "VÁLIDA"
    if value is False:
        return "NÃO CALCULADA (esperado no modo atual)"
    return "N/A"


def _fft_metric(value: object, fft_valid: bool | None, decimals: int, unit: str = "") -> str:
    if fft_valid is False:
        return "N/A (FFT desativada)"
    return _fmt(value, decimals, unit)
