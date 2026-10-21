"""Captura contextual de telemetria e indicadores de rede (fase 2).

O EventBus apenas deposita observações em buffers limitados; CSVs são gravados
fora do loop principal em lotes imutáveis e com rename atômico. O banco guarda
metadados e referências, nunca o conteúdo integral de telemetria.
"""
from __future__ import annotations

import csv
import hashlib
import json
import os
import re
import threading
import time
import uuid
from collections import defaultdict, deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import TYPE_CHECKING, Any

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (BusStatsReceived, PhysicalNodeReceived, TelemetryReceived,
                                  WirelessAssociationReceived, WirelessLinkReceived)
from pico_tui.services.log_manager import ACOUSTIC_COLUMNS

if TYPE_CHECKING:
    from pico_tui.history.service import HistoryService

PRE_SECONDS = 60.0
POST_SECONDS = 120.0
MAX_SECONDS = 600.0
MAX_ACTIVE = 8
MAX_RECENT_PER_STREAM = 1200
MAX_PENDING_PER_CAPTURE = 12000
CHUNK_INTERVAL_SECONDS = 30.0
CHUNK_MAX_ROWS = 500

TELEMETRY_COLUMNS = (
    'epoch','logical_id','sensor_timestamp_ms','gateway_timestamp_ms','can_timestamp_ms',
    'sequence','mode','acquisition','sample_rate_requested_hz','sample_rate_effective_hz',
    'window_type','window_size','rms','rms_unit','kurtosis','crest_factor',
    'peak_frequency_hz','peak_amplitude','spectral_entropy','fft_valid','axis',
    'ppv_mm_s','stalta_triggered','clipping','dtc_code','dtc_count','quality',
    'battery_percentage','battery_voltage_v',*ACOUSTIC_COLUMNS,'octave_db',
)
BUS_COLUMNS = ('epoch','window_ms','frames','bits','load_percent','peak_percent',
               'rx_errors','tx_errors','error_flags','rx_queue_peak','ids',
               'transfers_ok','transfers_err')
LINK_COLUMNS = ('epoch','logical_id','state','rssi_dbm','rtt_ms','rtt_max_ms',
                'lost_datagrams','auth_failures','replay_drops','rx_datagrams')
NODE_COLUMNS = ('epoch','node_id','state','role','can','wifi','firmware')
HEADERS = {'TELEMETRY': TELEMETRY_COLUMNS, 'BUS': BUS_COLUMNS,
           'LINK': LINK_COLUMNS, 'NODE': NODE_COLUMNS}
Stream = tuple[str, str]


def _str(value: Any) -> Any:
    return getattr(value, 'value', value)


def telemetry_row(sample: Any) -> dict[str, Any]:
    acoustic = getattr(sample, 'acoustic', None)
    battery = getattr(sample, 'battery', None)
    row = {
        'epoch': sample.received_wall_time,
        'logical_id': sample.logical_id,
        'sensor_timestamp_ms': sample.sensor_timestamp_ms,
        'gateway_timestamp_ms': sample.gateway_timestamp_ms,
        'can_timestamp_ms': sample.can_timestamp_ms,
        'sequence': sample.sequence,
        'mode': _str(sample.mode),
        'acquisition': _str(sample.acquisition_mode),
        'sample_rate_requested_hz': sample.sample_rate_requested_hz,
        'sample_rate_effective_hz': sample.sample_rate_effective_hz,
        'window_type': sample.window_type,
        'window_size': sample.window_size,
        'rms': sample.rms,
        'rms_unit': sample.rms_unit,
        'kurtosis': sample.kurtosis,
        'crest_factor': sample.crest_factor,
        'peak_frequency_hz': sample.peak_frequency_hz,
        'peak_amplitude': sample.peak_amplitude,
        'spectral_entropy': sample.spectral_entropy,
        'fft_valid': sample.fft_valid,
        'axis': sample.axis,
        'ppv_mm_s': sample.ppv_mm_s,
        'stalta_triggered': sample.stalta_triggered,
        'clipping': sample.clipping,
        'dtc_code': sample.dtc_code,
        'dtc_count': sample.dtc_count,
        'quality': _str(sample.quality),
        'battery_percentage': getattr(battery, 'percentage', None),
        'battery_voltage_v': getattr(battery, 'voltage_v', None),
    }
    for name in ACOUSTIC_COLUMNS:
        row[name] = getattr(acoustic, name, None) if acoustic is not None else None
    row['octave_db'] = (';'.join('' if v is None else str(v) for v in acoustic.octave_db)
                        if acoustic is not None else None)
    return row


def decide_scope(event: dict[str, Any]) -> tuple[str, str] | None:
    """Dispara somente para ocorrências operacionais relevantes; não para logs periódicos."""
    kind = event['kind']
    detail = event.get('details') or {}
    node = event.get('node_id')
    sensor = event.get('sensor_id')
    severity = event['severity']
    if kind == 'DTC' and severity in ('WARNING','CRITICAL') and sensor:
        return 'SENSOR', sensor
    if kind == 'LINK_STATE' and sensor:
        current = (detail.get('link_at_observation') or {}).get('state', '')
        if detail.get('previous') == 'SECURE' and current != 'SECURE':
            return 'NODE', str(node)
        if current in ('DOWN','NO_KEY'):
            return 'NODE', str(node)
    if kind == 'ASSOCIATION' and detail.get('state') in ('STALE','LOST','REJECTED'):
        return ('NODE', str(node)) if node is not None else ('NETWORK', '*')
    if kind in ('FAILOVER', 'LEADER_FAILURE_DETECTED', 'ELECTION_REQUESTED',
                'CAN_HIGH_LOAD', 'CAN_BUS_OFF', 'CAN_BUS_RECOVERED'):
        return 'NETWORK', '*'
    if kind == 'TRANSFER_FAILED':
        return ('NODE', str(node)) if node is not None else ('NETWORK', '*')
    if kind == 'FAULT_STARTED':
        return ('NODE', str(node)) if node is not None else ('NETWORK', '*')
    return None


@dataclass
class ActiveCapture:
    id: str
    scope: str
    target: str
    trigger_kind: str
    start_epoch: float
    trigger_epoch: float
    end_epoch: float
    pending: dict[Stream, deque[dict[str, Any]]] = field(default_factory=lambda: defaultdict(deque))
    file_seq: dict[Stream, int] = field(default_factory=dict)
    last_write_epoch: dict[Stream, float] = field(default_factory=dict)
    dropped: int = 0
    written: int = 0
    ended: bool = False


class CaptureManager:
    def __init__(self, bus: EventBus, history: HistoryService, *,
                 pre_seconds: float = PRE_SECONDS, post_seconds: float = POST_SECONDS,
                 max_seconds: float = MAX_SECONDS, max_active: int = MAX_ACTIVE) -> None:
        if not (0 <= pre_seconds <= max_seconds and 0 < post_seconds <= max_seconds):
            raise ValueError('Janela de captura inválida')
        self.history = history
        self.store = history.store
        self.pre_seconds, self.post_seconds = pre_seconds, post_seconds
        self.max_seconds, self.max_active = max_seconds, max_active
        self.recent: dict[Stream, deque[dict[str, Any]]] = defaultdict(
            lambda: deque(maxlen=MAX_RECENT_PER_STREAM))
        self.active: dict[str, ActiveCapture] = {}
        self.sensor_nodes: dict[str, int] = {}
        self.rejected = 0
        self._lock = threading.RLock()
        self._flush_lock = threading.Lock()
        history.capture_manager = self
        for typ, fn in (
            (TelemetryReceived, self._telemetry),
            (BusStatsReceived, self._bus),
            (WirelessLinkReceived, self._link),
            (PhysicalNodeReceived, self._node),
            (WirelessAssociationReceived, self._association),
        ):
            bus.subscribe(typ, fn)

    def _association(self, ev: WirelessAssociationReceived) -> None:
        if ev.child_id <= 0:
            return
        logical = f'{ev.parent_node_id:02d}.{ev.child_id:02d}'
        if ev.state.upper() == 'UNBOUND':
            self.sensor_nodes.pop(logical, None)
        else:
            self.sensor_nodes[logical] = ev.parent_node_id

    def _telemetry(self, ev: TelemetryReceived) -> None:
        row = telemetry_row(ev.sample)
        self.sensor_nodes[ev.sample.logical_id] = ev.sample.parent_node_id
        self._observe(('TELEMETRY', ev.sample.logical_id), row)

    def _bus(self, ev: BusStatsReceived) -> None:
        row = {k: getattr(ev, k, None) for k in BUS_COLUMNS if k != 'epoch'}
        row['epoch'] = time.time()
        self._observe(('BUS', ''), row)

    def _link(self, ev: WirelessLinkReceived) -> None:
        ident = f'{ev.parent_node_id:02d}.{ev.child_id:02d}'
        self.sensor_nodes[ident] = ev.parent_node_id
        row = {k: getattr(ev, k) for k in LINK_COLUMNS if k not in ('epoch', 'logical_id')}
        row.update(epoch=time.time(), logical_id=ident)
        self._observe(('LINK', ident), row)

    def _node(self, ev: PhysicalNodeReceived) -> None:
        if ev.parent_node_id <= 0:
            return
        payload = ev.payload
        row = {'epoch': time.time(), 'node_id': ev.parent_node_id,
               'state': payload.get('STATE', payload.get('STATUS', '')),
               'role': payload.get('ROLE', ''), 'can': payload.get('CAN', ''),
               'wifi': payload.get('WIFI', ''), 'firmware': payload.get('FIRMWARE', '')}
        self._observe(('NODE', str(ev.parent_node_id)), row)

    def _include(self, capture: ActiveCapture, key: Stream) -> bool:
        kind, ident = key
        if kind == 'BUS' or capture.scope == 'NETWORK':
            return True
        if kind == 'NODE':
            if capture.scope == 'NODE':
                return ident == capture.target
            if capture.scope == 'SENSOR':
                return int(ident) == self.sensor_nodes.get(capture.target, -1)
            return False
        if capture.scope == 'SENSOR':
            return ident == capture.target
        if capture.scope == 'NODE':
            return self.sensor_nodes.get(ident) == int(capture.target)
        return False

    def _observe(self, key: Stream, row: dict[str, Any]) -> None:
        with self._lock:
            epoch = float(row['epoch'])
            ring = self.recent[key]
            ring.append(row)
            cutoff = epoch - self.pre_seconds
            while ring and float(ring[0]['epoch']) < cutoff:
                ring.popleft()
            for capture in self.active.values():
                if not capture.ended and capture.start_epoch <= epoch <= capture.end_epoch and self._include(capture, key):
                    self._queue_row(capture, key, row)

    @staticmethod
    def _queue_row(capture: ActiveCapture, key: Stream, row: dict[str, Any]) -> None:
        count = sum(map(len, capture.pending.values()))
        if count >= MAX_PENDING_PER_CAPTURE:
            capture.dropped += 1
            return
        capture.pending[key].append(row)

    def on_history_event(self, event: dict[str, Any]) -> list[str]:
        """Invocado pelo HistoryService antes de enfileirar o evento SQL."""
        with self._lock:
            epoch = float(event['epoch'])
            triggered = decide_scope(event)
            ids: list[str] = []
            for cap in self.active.values():
                if cap.ended or not (cap.start_epoch <= epoch <= cap.end_epoch):
                    continue
                if (cap.scope == 'NETWORK' or
                    (cap.scope == 'NODE' and str(event.get('node_id')) == cap.target) or
                    (cap.scope == 'SENSOR' and event.get('sensor_id') == cap.target)):
                    ids.append(cap.id)
            if triggered:
                scope, target = triggered
                existing = next((cap for cap in self.active.values() if not cap.ended and
                                 cap.scope == scope and cap.target == target and
                                 epoch <= cap.end_epoch), None)
                if existing:
                    previous_end = existing.end_epoch
                    existing.end_epoch = min(existing.trigger_epoch + self.max_seconds,
                                             max(existing.end_epoch, epoch + self.post_seconds))
                    if existing.end_epoch != previous_end:
                        self.history._push('capture_extend', {'id': existing.id,
                                                              'end_target_epoch': existing.end_epoch})
                    if existing.id not in ids:
                        ids.append(existing.id)
                elif sum(not x.ended for x in self.active.values()) < self.max_active:
                    cid = uuid.uuid4().hex[:20]
                    cap = ActiveCapture(cid, scope, target, event['kind'],
                                        epoch - self.pre_seconds, epoch,
                                        epoch + self.post_seconds)
                    self.active[cid] = cap
                    self.history._push('capture_start', {
                        'id': cid, 'scope': scope, 'target': target,
                        'trigger_epoch': epoch, 'started_epoch': cap.start_epoch,
                        'end_target_epoch': cap.end_epoch, 'reason': event['kind'],
                    })
                    # Recupera dados anteriores, dentro do escopo e da janela.
                    for key, ring in self.recent.items():
                        if self._include(cap, key):
                            for row in ring:
                                if cap.start_epoch <= row['epoch'] <= epoch:
                                    self._queue_row(cap, key, row)
                    ids.append(cid)
                else:
                    self.rejected += 1
                    event.setdefault('details', {})['capture_status'] = 'MAX_ACTIVE_REACHED'
            return ids

    def flush(self, *, finish: bool = False, force: bool = False, now: float | None = None) -> None:
        """Executado em worker. Arquivos são chunks CSV atômicos e verificáveis."""
        if not self._flush_lock.acquire(blocking=finish):
            return
        try:
            timestamp = time.time() if now is None else now
            with self._lock:
                jobs: list[tuple[ActiveCapture, Stream, list[dict[str, Any]], int]] = []
                for cap in self.active.values():
                    for key, data in cap.pending.items():
                        if data and (finish or force or timestamp >= cap.end_epoch or len(data) >= CHUNK_MAX_ROWS or
                                     timestamp - cap.last_write_epoch.get(key, cap.trigger_epoch)
                                     >= CHUNK_INTERVAL_SECONDS):
                            jobs.append((cap, key, list(data), cap.file_seq.get(key, 0) + 1))
            for cap, key, data, seq in jobs:
                kind, ident = key
                slug = re.sub(r'[^A-Za-z0-9_-]+', '_', ident) or 'GLOBAL'
                stamp = datetime.fromtimestamp(cap.trigger_epoch, timezone.utc).strftime('%Y-%m-%d')
                rel = Path('capturas') / stamp / cap.id / f'{kind.lower()}_{slug}_{seq:06d}.csv'
                destination = self.store.path.parent / rel
                destination.parent.mkdir(parents=True, exist_ok=True)
                tmp = destination.with_suffix('.csv.tmp')
                with tmp.open('w', encoding='utf-8', newline='') as handle:
                    writer = csv.DictWriter(handle, fieldnames=HEADERS[kind], extrasaction='ignore')
                    writer.writeheader()
                    writer.writerows(data)
                    handle.flush()
                    os.fsync(handle.fileno())
                os.replace(tmp, destination)
                blob = destination.read_bytes()
                meta = {
                    'capture_id': cap.id, 'relative_path': rel.as_posix(),
                    'kind': kind, 'sensor_id': ident or None,
                    'rows': len(data), 'first_epoch': min(row['epoch'] for row in data),
                    'last_epoch': max(row['epoch'] for row in data),
                    'sha256': hashlib.sha256(blob).hexdigest(), 'bytes': len(blob),
                }
                # Idempotente: após erro SQL, reescreve o mesmo chunk, sem duplicá-lo.
                self.store.upsert_capture_file(meta)
                with self._lock:
                    queue = cap.pending[key]
                    for _ in range(len(data)):
                        queue.popleft()
                    cap.written += len(data)
                    cap.file_seq[key] = seq
                    cap.last_write_epoch[key] = timestamp
            with self._lock:
                to_finish = [cap for cap in self.active.values()
                             if not cap.ended and (finish or timestamp >= cap.end_epoch)]
            for cap in to_finish:
                state = ('PARTIAL' if (cap.dropped or (finish and timestamp < cap.end_epoch))
                         else 'COMPLETE' if cap.written else 'EMPTY')
                self.store.finish_capture(cap.id, ended_epoch=min(timestamp, cap.end_epoch),
                                          dropped_records=cap.dropped, state=state)
                with self._lock:
                    cap.ended = True
                    del self.active[cap.id]
        finally:
            self._flush_lock.release()

    def close(self) -> None:
        self.flush(finish=True)
