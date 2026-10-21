"""Etapa 2: janelas pre/post, SQL↔CSV, integridade, deduplicação e escopo."""
from __future__ import annotations

import asyncio
import csv
import time
from pathlib import Path

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (BusStatsReceived, DtcReceived, NetworkEventReceived,
                                  TelemetryReceived, WirelessAssociationReceived,
                                  WirelessLinkReceived)
from pico_tui.core.models import DtcRecord, Severity, TelemetrySample
from pico_tui.history.captures import CaptureManager
from pico_tui.history.service import HistoryService
from pico_tui.history.storage import HistoryStore


def start(tmp_path: Path, **kwargs):
    bus = EventBus()
    store = HistoryStore(tmp_path / 'logs' / 'historico.sqlite3')
    history = HistoryService(bus, store, 'test-session', origin='TEST')
    cap = CaptureManager(bus, history, **kwargs)
    return bus, store, history, cap


def emit(bus: EventBus, *events) -> None:
    async def push():
        for event in events:
            await bus.publish(event)
    asyncio.run(push())


def tel(child: int, *, node: int = 1, earlier: float = 0.0, rms: float = 2.0):
    return TelemetryReceived(TelemetrySample(node, child, received_wall_time=time.time() - earlier,
                                              rms=rms, sequence=int(rms * 10)))


def bus_sample(load: float = 4):
    return BusStatsReceived(1000,1000,10,1200,load,load*1.1)


def finalize(history: HistoryService, cap: CaptureManager) -> None:
    history.flush()  # captura inicial + evento devem existir antes de gravar arquivos
    cap.close()
    history.close()


def test_critical_dtc_records_pre_and_post_telemetry_as_external_csv(tmp_path):
    bus, store, history, cap = start(tmp_path)
    emit(bus, tel(1, earlier=3, rms=1.2), bus_sample())
    emit(bus, DtcReceived(1,1,DtcRecord(0x2002,severity=Severity.CRITICAL)))
    emit(bus, tel(1, rms=3.1), bus_sample(5))
    finalize(history, cap)
    record = next(e for e in store.list_events('test-session') if e.kind == 'DTC')
    captures = store.capture_for_event(record.id)
    assert len(captures) == 1 and captures[0]['state'] in ('COMPLETE', 'PARTIAL')
    files = store.files_for_event(record.id)
    assert any(f.kind == 'TELEMETRY' and f.sensor_id == '01.01' for f in files)
    assert any(f.kind == 'BUS' for f in files)
    tel_file = next(f for f in files if f.kind == 'TELEMETRY')
    status, preview = store.preview_csv(tel_file)
    assert status == 'OK' and [float(row['rms']) for row in preview] == [1.2, 3.1]
    assert store.path.stat().st_size < 1_000_000  # telemetria não está no banco


def test_repeated_events_same_scope_share_capture_and_multiple_events_reference_files(tmp_path):
    bus, store, history, cap = start(tmp_path)
    emit(bus, tel(1), DtcReceived(1,1,DtcRecord(4,severity=Severity.WARNING)),
         DtcReceived(1,1,DtcRecord(5,severity=Severity.CRITICAL)))
    finalize(history, cap)
    dtcs = [e for e in store.list_events('test-session') if e.kind == 'DTC']
    assert len(dtcs) == 2
    ids = [store.capture_for_event(e.id)[0]['id'] for e in dtcs]
    assert ids[0] == ids[1]
    assert store.files_for_event(dtcs[0].id)
    assert store.files_for_event(dtcs[1].id)


def test_network_capture_includes_both_nodes_and_multiple_sensors(tmp_path):
    bus, store, history, cap = start(tmp_path)
    emit(bus, tel(1,node=1), tel(1,node=2), bus_sample(),
         NetworkEventReceived('ELECTION_REQUESTED',1))
    finalize(history, cap)
    event = next(e for e in store.list_events('test-session') if e.kind == 'ELECTION_REQUESTED')
    files = store.files_for_event(event.id)
    ids = {f.sensor_id for f in files if f.kind == 'TELEMETRY'}
    assert ids == {'01.01','02.01'}
    assert store.capture_intervals('test-session',event.epoch-60,event.epoch+120)


def test_link_state_transition_uses_node_scope(tmp_path):
    bus, store, history, cap = start(tmp_path)
    emit(bus, tel(1,node=2), tel(2,node=2), tel(1,node=3),
         WirelessLinkReceived(2,1,'SECURE'),WirelessLinkReceived(2,1,'DOWN'))
    finalize(history,cap)
    event = next(e for e in store.list_events('test-session') if e.kind == 'LINK_STATE' and e.title.endswith('DOWN'))
    files = store.files_for_event(event.id)
    assert {f.sensor_id for f in files if f.kind == 'TELEMETRY'} == {'02.01','02.02'}
    assert store.capture_for_event(event.id)[0]['scope'] == 'NODE'


def test_event_without_relevant_trigger_keeps_database_but_does_not_create_capture(tmp_path):
    bus, store, history, cap = start(tmp_path)
    emit(bus, DtcReceived(1,1,DtcRecord(7,severity=Severity.INFO)))
    finalize(history,cap)
    event = store.list_events('test-session')[0]
    assert event.kind == 'DTC'
    assert store.capture_for_event(event.id) == []


def test_missing_or_modified_csv_is_reported_without_inventing_telemetry(tmp_path):
    bus, store, history, cap = start(tmp_path)
    emit(bus, tel(1), DtcReceived(1,1,DtcRecord(10,severity=Severity.WARNING)))
    finalize(history,cap)
    event = store.list_events('test-session')[0]
    item = next(f for f in store.files_for_event(event.id) if f.kind == 'TELEMETRY')
    path = store.path.parent / item.relative_path
    path.write_text('conteudo adulterado',encoding='utf-8')
    assert store.preview_csv(item)[0] == 'INTEGRIDADE_INVALIDA'
    path.unlink()
    assert store.preview_csv(item)[0] == 'AUSENTE'


def test_capture_limit_does_not_drop_event_records(tmp_path):
    bus, store, history, cap = start(tmp_path,max_active=1)
    emit(bus, DtcReceived(1,1,DtcRecord(4,severity=Severity.CRITICAL)),
         DtcReceived(2,1,DtcRecord(5,severity=Severity.CRITICAL)))
    finalize(history,cap)
    assert store.count_events('test-session') == 2
    assert cap.rejected == 1
    assert len([e for e in store.list_events('test-session') if store.capture_for_event(e.id)]) == 1


def test_storage_reopens_legacy_phase1_and_creates_capture_tables(tmp_path):
    import sqlite3
    db = tmp_path/'logs'/'history.db'
    db.parent.mkdir()
    with sqlite3.connect(db) as cx:
        cx.execute('CREATE TABLE sessions(id TEXT PRIMARY KEY,started_epoch REAL,ended_epoch REAL,origin TEXT,port TEXT)')
        cx.execute('CREATE TABLE events(id INTEGER PRIMARY KEY,session_id TEXT,epoch REAL,kind TEXT,category TEXT,severity TEXT,title TEXT,node_id INTEGER,sensor_id TEXT,details_json TEXT)')
    store = HistoryStore(db)
    with sqlite3.connect(db) as cx:
        assert cx.execute("SELECT name FROM sqlite_master WHERE name='capture_events'").fetchone()
    assert store.list_sessions() == []


def test_batch_failure_preserves_rows_for_retry(tmp_path, monkeypatch):
    bus, store, history, cap = start(tmp_path)
    emit(bus, tel(1), DtcReceived(1,1,DtcRecord(25,severity=Severity.WARNING)))
    history.flush()
    original = store.upsert_capture_file
    calls = 0

    def fail_once(record):
        nonlocal calls
        calls += 1
        if calls == 1:
            raise OSError('simulacao de falha no banco')
        return original(record)

    monkeypatch.setattr(store, 'upsert_capture_file', fail_once)
    try:
        cap.flush(force=True)
    except OSError:
        pass
    else:
        raise AssertionError('Era esperada uma falha de armazenamento')
    cap.close()
    history.close()
    event = store.list_events('test-session')[0]
    assert store.files_for_event(event.id)
    assert all(store.preview_csv(f)[0] == 'OK' for f in store.files_for_event(event.id))


def test_new_samples_after_flush_create_new_immutable_chunk(tmp_path):
    bus, store, history, cap = start(tmp_path)
    emit(bus, tel(1,rms=1), DtcReceived(1,1,DtcRecord(14,severity=Severity.WARNING)))
    history.flush()
    cap.flush(force=True)
    emit(bus,tel(1,rms=2))
    cap.close()
    history.close()
    event=store.list_events('test-session')[0]
    telemetry_files=[f for f in store.files_for_event(event.id) if f.kind=='TELEMETRY']
    assert len(telemetry_files)==2
    assert sum(f.rows for f in telemetry_files)==2
    assert len({f.relative_path for f in telemetry_files})==2


def test_time_expired_capture_flushes_pending_even_before_chunk_interval(tmp_path):
    bus, store, history, cap = start(tmp_path,post_seconds=1.0)
    emit(bus, tel(1), DtcReceived(1,1,DtcRecord(51,severity=Severity.WARNING)))
    history.flush()
    event = store.list_events('test-session')[0]
    cap.flush(now=event.epoch+3)  # fim da janela força a gravação do último fragmento
    assert store.files_for_event(event.id)
    assert store.capture_for_event(event.id)[0]['state'] == 'COMPLETE'
    history.close()
