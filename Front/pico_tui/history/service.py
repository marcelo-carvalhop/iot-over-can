"""Assina eventos do domínio e produz registros históricos de baixo volume.

A escrita SQL nunca ocorre nos callbacks do EventBus: os eventos vão para uma
fila limitada e são descarregados em lotes por um worker da aplicação.
"""
from __future__ import annotations

import re
import threading
import time
from collections import deque
from dataclasses import asdict
from typing import Any

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    BusStatsReceived,
    CommandAck,
    ConfigurationApplied,
    ConnectionClosed,
    ConnectionOpened,
    DtcCleared,
    DtcReceived,
    FaultEventReceived,
    LogEvent,
    NetworkEventReceived,
    PanelReported,
    TransferFailed,
    WirelessAssociationReceived,
    WirelessFailoverReceived,
    WirelessLinkReceived,
)
from pico_tui.history.storage import HistoryStore
from pico_tui.services.recorder import redact

BUS_SAMPLE_INTERVAL_S = 30.0
MAX_QUEUE = 5000


TAG_RE = re.compile(r"#([^\W_][\w-]{0,31})", re.UNICODE)


def mark_tags(text: str) -> list[str]:
    """Etiquetas (#cabo, #ensaio3) de uma nota do operador, sem repetição e em minúsculas."""
    seen: list[str] = []
    for tag in TAG_RE.findall(text or ""):
        tag = tag.lower().rstrip("-")
        if tag and tag not in seen:
            seen.append(tag)
    return seen


class HistoryService:
    def __init__(self, bus: EventBus, store: HistoryStore, session_id: str,
                 *, origin: str = "LIVE", port: str = "") -> None:
        self.bus, self.store, self.session_id = bus, store, session_id
        self.started = store.start_session(session_id, origin=origin, port=port)
        self.queue: deque[tuple[str, dict[str, Any]]] = deque()
        self.dropped = 0
        self.last_bus: dict[str, Any] | None = None
        self.last_link: dict[str, dict[str, Any]] = {}
        self.association_states: dict[tuple[int, str], str] = {}
        self.dtc_states: dict[tuple[int, int, int], tuple[bool, int, str]] = {}
        self.last_leader: int | None = None
        self.bus_window_start: float | None = None
        self.bus_count = 0
        self.bus_sum_load = 0.0
        self.bus_total_window_ms = 0
        self.bus_peak = 0.0
        self.bus_frames = 0
        self.bus_errors = (0, 0, 0)
        self._high_load_count = 0
        self._normal_load_count = 0
        self._can_high_load = False
        self._can_bus_off = False
        self.closed = False
        self.capture_manager = None  # registrado pela camada de capturas (fase 2)
        self._flush_lock = threading.Lock()
        self._queue_lock = threading.Lock()
        for event_type, handler in (
            (BusStatsReceived, self._on_bus),
            (WirelessLinkReceived, self._on_link),
            (WirelessAssociationReceived, self._on_association),
            (WirelessFailoverReceived, self._on_failover),
            (NetworkEventReceived, self._on_network),
            (FaultEventReceived, self._on_fault),
            (DtcReceived, self._on_dtc),
            (DtcCleared, self._on_dtc_clear),
            (ConfigurationApplied, self._on_config),
            (ConnectionOpened, self._on_connection_open),
            (ConnectionClosed, self._on_connection),
            (TransferFailed, self._on_transfer_failed),
            (CommandAck, self._on_command_ack),
            (LogEvent, self._on_log),
            (PanelReported, self._on_panel),
        ):
            bus.subscribe(event_type, handler)

    def _push(self, category: str, entry: dict[str, Any]) -> None:
        # Os callbacks podem concorrer com flush(), executado num worker.
        with self._queue_lock:
            if self.closed:
                return
            if len(self.queue) >= MAX_QUEUE:
                self.queue.popleft()
                self.dropped += 1
            self.queue.append((category, entry))

    def _event(self, kind: str, category: str, severity: str, title: str,
               *, node_id: int | None = None, sensor_id: str | None = None,
               details: dict[str, Any] | None = None, epoch: float | None = None) -> None:
        if self.closed:
            return
        payload = dict(details or {})
        if self.last_bus is not None:
            payload["bus_at_observation"] = self.last_bus.copy()
        if sensor_id and sensor_id in self.last_link:
            payload["link_at_observation"] = self.last_link[sensor_id].copy()
        record = {
            "epoch": time.time() if epoch is None else epoch, "kind": kind, "category": category,
            "severity": severity, "title": title,
            "node_id": node_id, "sensor_id": sensor_id, "details": payload,
        }
        if self.capture_manager is not None:
            record["capture_ids"] = self.capture_manager.on_history_event(record)
        self._push("event", record)

    def _on_bus(self, ev: BusStatsReceived) -> None:
        now = time.time()
        self.last_bus = {
            "timestamp_epoch": now, "load_percent": ev.load_percent,
            "peak_percent": ev.peak_percent, "frames": ev.frames,
            "rx_errors": ev.rx_errors, "tx_errors": ev.tx_errors,
            "error_flags": ev.error_flags,
        }
        if self.bus_window_start is None:
            self.bus_window_start = now
        self.bus_count += 1
        window_ms = max(1, ev.window_ms)
        self.bus_sum_load += ev.load_percent * window_ms
        self.bus_total_window_ms += window_ms
        self.bus_peak = max(self.bus_peak, ev.peak_percent)
        self.bus_frames += ev.frames
        self.bus_errors = (ev.rx_errors, ev.tx_errors, ev.error_flags)
        self._high_load_count = self._high_load_count + 1 if ev.load_percent >= 40.0 else 0
        self._normal_load_count = self._normal_load_count + 1 if ev.load_percent <= 35.0 else 0
        if not self._can_high_load and self._high_load_count >= 3:
            self._can_high_load = True
            self._event("CAN_HIGH_LOAD", "CAN", "WARNING", "Ocupação CAN elevada por três janelas")
        elif self._can_high_load and self._normal_load_count >= 3:
            self._can_high_load = False
            self._event("CAN_LOAD_RECOVERED", "CAN", "INFO", "Ocupação CAN normalizada")
        is_bus_off = bool(ev.error_flags & 0x20)
        if is_bus_off != self._can_bus_off:
            self._can_bus_off = is_bus_off
            self._event("CAN_BUS_OFF" if is_bus_off else "CAN_BUS_RECOVERED", "CAN",
                        "CRITICAL" if is_bus_off else "INFO",
                        "Barramento em BUS-OFF" if is_bus_off else "Controlador CAN recuperado")
        if now - self.bus_window_start >= BUS_SAMPLE_INTERVAL_S:
            self._flush_bus_window(now)

    def _flush_bus_window(self, now: float) -> None:
        if not self.bus_count:
            return
        rx, tx, flags = self.bus_errors
        self._push("bus", {
            "epoch": now, "samples": self.bus_count,
            "load_avg_pct": self.bus_sum_load / self.bus_total_window_ms,
            "peak_max_pct": self.bus_peak, "frames": self.bus_frames,
            "rx_errors": rx, "tx_errors": tx, "error_flags": flags,
        })
        self.bus_count = 0
        self.bus_window_start = None
        self.bus_sum_load = 0.0
        self.bus_total_window_ms = 0
        self.bus_peak = 0.0
        self.bus_frames = 0

    def _on_link(self, ev: WirelessLinkReceived) -> None:
        ident = f"{ev.parent_node_id:02d}.{ev.child_id:02d}"
        previous = self.last_link.get(ident)
        state = ev.state.upper()
        self.last_link[ident] = {
            "state": state, "rtt_ms": ev.rtt_ms, "rssi_dbm": ev.rssi_dbm,
            "lost_datagrams": ev.lost_datagrams,
            "auth_failures": ev.auth_failures, "replay_drops": ev.replay_drops,
        }
        sleep_cycle = state == "SLEEPING" or (previous is not None and previous["state"] == "SLEEPING" and state == "SECURE")
        if (previous is None or previous["state"] != state) and not sleep_cycle:
            severity = "WARNING" if state in ("DOWN", "NO_KEY") else "INFO"
            self._event("LINK_STATE", "WIRELESS", severity,
                        f"Enlace {ident}: {state}", node_id=ev.parent_node_id,
                        sensor_id=ident, details={"previous": previous["state"] if previous else None})

    def _on_association(self, ev: WirelessAssociationReceived) -> None:
        key = (ev.parent_node_id, ev.wireless_uuid)
        state = ev.state.upper()
        if self.association_states.get(key) == state:
            return
        self.association_states[key] = state
        self._event("ASSOCIATION", "SENSOR", "WARNING" if state in ("STALE", "LOST", "REJECTED") else "INFO",
                    f"Sensor {ev.wireless_uuid}: {state}", node_id=ev.parent_node_id,
                    sensor_id=f"{ev.parent_node_id:02d}.{ev.child_id:02d}" if ev.child_id > 0 else None,
                    details={"uuid": ev.wireless_uuid, "state": state, "profile": ev.profile_id,
                             "rssi_dbm": ev.rssi_dbm, "auto_failover": ev.failover_auto})

    def _on_failover(self, ev: WirelessFailoverReceived) -> None:
        self._event("FAILOVER", "NETWORK", "WARNING",
                    f"Sensor {ev.wireless_uuid}: Node {ev.from_node_id:02d} → {ev.to_node_id:02d}",
                    node_id=ev.to_node_id,
                    details={"uuid": ev.wireless_uuid, "previous_node": ev.from_node_id,
                             "new_node": ev.to_node_id, "reason": ev.reason, "rssi_dbm": ev.rssi_dbm})

    def _on_network(self, ev: NetworkEventReceived) -> None:
        if ev.kind in ("LEADER_OBSERVED", "LEADER_ANNOUNCED"):
            if self.last_leader == ev.node_id:
                return
            self.last_leader = ev.node_id
        self._event(ev.kind, "NETWORK", "WARNING" if "FAILURE" in ev.kind else "INFO",
                    f"Rede: {ev.kind.replace('_', ' ').lower()} (Node {ev.node_id:02d})",
                    node_id=ev.node_id or None, details={"detail": ev.detail})

    def _on_fault(self, ev: FaultEventReceived) -> None:
        self._event("FAULT_" + ev.state.upper(), "CAN", "WARNING" if ev.state.upper() == "STARTED" else "INFO",
                    f"Ensaio {ev.kind}: {ev.state}", node_id=ev.node_id or None,
                    details={"duration_ms": ev.duration_ms, "sequence": ev.seq, **ev.payload})

    def _on_dtc(self, ev: DtcReceived) -> None:
        record = ev.record
        raw_severity = getattr(record, "severity", "WARNING")
        severity = str(getattr(raw_severity, "value", raw_severity)).upper()
        if severity not in ("INFO", "WARNING", "CRITICAL"):
            severity = "WARNING"
        key = (ev.parent_node_id, ev.child_id, record.code)
        current = (bool(record.active), int(record.occurrence_count), severity)
        if self.dtc_states.get(key) == current:
            return
        self.dtc_states[key] = current
        ident = f"{ev.parent_node_id:02d}.{ev.child_id:02d}"
        self._event("DTC", "SENSOR", severity, f"DTC no sensor {ident}: 0x{record.code:04X}",
                    node_id=ev.parent_node_id, sensor_id=ident,
                    details={"code": record.code, "record": asdict(record)})

    def _on_dtc_clear(self, ev: DtcCleared) -> None:
        for key in list(self.dtc_states):
            if key[:2] == (ev.parent_node_id, ev.child_id) and (ev.code is None or key[2] == ev.code):
                del self.dtc_states[key]
        ident = f"{ev.parent_node_id:02d}.{ev.child_id:02d}"
        self._event("DTC_CLEARED", "SENSOR", "INFO", f"DTC limpo no sensor {ident}",
                    node_id=ev.parent_node_id, sensor_id=ident, details={"code": ev.code})

    def _on_config(self, ev: ConfigurationApplied) -> None:
        ident = f"{ev.parent_node_id:02d}.{ev.child_id:02d}"
        self._event("CONFIG_APPLIED", "SENSOR", "INFO", f"Configuração aplicada: sensor {ident}",
                    node_id=ev.parent_node_id, sensor_id=ident, details=ev.payload)

    def _on_connection_open(self, ev: ConnectionOpened) -> None:
        self._event("CONNECTION_OPENED", "NETWORK", "INFO", "Supervisão conectada",
                    details={"port": ev.port})

    def _on_transfer_failed(self, ev: TransferFailed) -> None:
        self._event("TRANSFER_FAILED", "CAN", "WARNING",
                    f"Transferência {ev.transfer_type} de {ev.source} falhou",
                    node_id=ev.source[0], sensor_id=f"{ev.source[0]:02d}.{ev.source[1]:02d}",
                    details={"reason": ev.reason, "transfer_id": ev.transfer_id})

    def _on_command_ack(self, ev: CommandAck) -> None:
        if ev.state.upper() in ("REJECTED", "NO_LINK", "ERROR", "TIMEOUT"):
            self._event("COMMAND_REJECTED", "NETWORK", "WARNING",
                        f"Comando {ev.command} não concluído: {ev.state}",
                        details={"transaction_id": ev.transaction_id})

    def _on_connection(self, ev: ConnectionClosed) -> None:
        self._event("CONNECTION_CLOSED", "NETWORK", "WARNING", "Conexão de supervisão encerrada",
                    details={"port": ev.port, "reason": ev.reason})

    def _on_panel(self, ev: PanelReported) -> None:
        """Marcas do operador vão para a faixa OPERATOR, no instante em que foram feitas."""
        ident = f"{ev.parent_node_id:02d}.{ev.child_id:02d}"
        if ev.kind == "MARK":
            kind = ev.payload.get("KIND", "")
            title = {"BUTTON": "Intervenção física", "STEP": "Passo de roteiro",
                     "RESULT": "Resultado de roteiro", "FLAG": "Marca do operador",
                     "ACK": "Alerta reconhecido"}.get(kind, "Nota do operador")
            details: dict[str, Any] = {"kind": kind, "step": ev.payload.get("STEP"), "seq": ev.payload.get("SEQ"),
                                       "text": ev.text, "age_ms": ev.age_ms}
            tags = mark_tags(ev.text)
            if tags:
                details["tags"] = tags
            self._event("OPERATOR_" + (kind or "MARK"), "OPERATOR", "INFO",
                        f"{title} (painel {ident})" + (f": {ev.text}" if ev.text else ""),
                        node_id=ev.parent_node_id, sensor_id=ident, epoch=time.time() - ev.age_ms / 1000.0,
                        details=details)
        elif ev.kind == "STATUS" and ev.payload.get("STATE", "").upper() == "OFF":
            self._event("PANEL_OFF", "OPERATOR", "INFO", f"Painel {ident} desligado pelo operador",
                        node_id=ev.parent_node_id, sensor_id=ident, details=dict(ev.payload))

    def _on_log(self, ev: LogEvent) -> None:
        if ev.level.upper() not in ("WARNING", "ERROR", "CRITICAL"):
            return
        if ev.source == "INTERNO":
            return
        severity = "CRITICAL" if ev.level.upper() in ("ERROR", "CRITICAL") else "WARNING"
        self._event("LOG_" + ev.source.upper(), "SYSTEM", severity,
                    redact(ev.message)[:240], details={"source": ev.source})

    def flush(self) -> None:
        """O chamador deve executar esta operação fora do loop principal da TUI."""
        with self._flush_lock:
            # Desanexar o lote atomicamente: eventos recebidos durante o SQL
            # permanecem na fila para o flush seguinte.
            with self._queue_lock:
                if not self.queue and not self.dropped:
                    return
                batch = list(self.queue)
                self.queue.clear()
                lost = self.dropped
                self.dropped = 0
            if lost:
                batch.append(("event", {
                    "epoch": time.time(), "kind": "HISTORY_QUEUE_OVERFLOW",
                    "category": "SYSTEM", "severity": "WARNING",
                    "title": f"Fila histórica descartou {lost} registros",
                    "details": {"dropped": lost},
                }))
            try:
                self.store.write_batch(self.session_id, batch)
            except Exception:
                # Em falha de I/O, devolve o lote à frente dos eventos novos.
                # A fila continua limitada; descartes são contabilizados.
                old_batch = batch[:len(batch) - (1 if lost else 0)]
                with self._queue_lock:
                    restored = deque(old_batch)
                    restored.extend(self.queue)
                    while len(restored) > MAX_QUEUE:
                        restored.popleft()
                        self.dropped += 1
                    self.queue = restored
                    self.dropped += lost
                raise

    def close(self) -> None:
        if self.closed:
            return
        self._flush_bus_window(time.time())
        with self._queue_lock:
            self.closed = True
        self.flush()
        self.store.finish_session(self.session_id)
