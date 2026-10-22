"""Regressões de flush concorrente e de retomada após falha de gravação."""
from __future__ import annotations

import threading

from pico_tui.core.event_bus import EventBus
from pico_tui.history.service import HistoryService
from pico_tui.history.storage import HistoryStore


def test_event_arriving_while_sql_flush_is_running_is_not_lost(tmp_path, monkeypatch):
    store = HistoryStore(tmp_path / "history.sqlite3")
    history = HistoryService(EventBus(), store, "flush-race", origin="TEST")
    started = threading.Event()
    release = threading.Event()
    original = store.write_batch
    errors = []

    def blocked_write(session_id, batch):
        started.set()
        assert release.wait(3), "Flush não liberado"
        return original(session_id, batch)

    monkeypatch.setattr(store, "write_batch", blocked_write)
    history._event("FIRST", "SYSTEM", "INFO", "Primeiro evento")

    def worker():
        try:
            history.flush()
        except Exception as exc:
            errors.append(exc)

    thread = threading.Thread(target=worker)
    thread.start()
    try:
        assert started.wait(3), "Flush não iniciou"
        history._event("SECOND", "SYSTEM", "INFO", "Evento durante o flush")
    finally:
        release.set()
        thread.join(timeout=5)
    assert not thread.is_alive()
    assert not errors
    history.close()
    assert [record.kind for record in store.list_events("flush-race")] == ["FIRST", "SECOND"]


def test_failed_flush_restores_older_events_before_new_events(tmp_path, monkeypatch):
    store = HistoryStore(tmp_path / "history.sqlite3")
    history = HistoryService(EventBus(), store, "flush-error", origin="TEST")
    started = threading.Event()
    release = threading.Event()
    original = store.write_batch
    calls = 0
    errors = []

    def fail_first(session_id, batch):
        nonlocal calls
        calls += 1
        if calls == 1:
            started.set()
            assert release.wait(3)
            raise OSError("falha de escrita simulada")
        return original(session_id, batch)

    monkeypatch.setattr(store, "write_batch", fail_first)
    history._event("FIRST", "SYSTEM", "INFO", "Antes do flush")

    def worker():
        try:
            history.flush()
        except Exception as exc:
            errors.append(exc)

    thread = threading.Thread(target=worker)
    thread.start()
    try:
        assert started.wait(3)
        history._event("SECOND", "SYSTEM", "INFO", "Durante o flush com falha")
    finally:
        release.set()
        thread.join(timeout=5)
    assert len(errors) == 1 and isinstance(errors[0], OSError)
    history.close()
    assert [record.kind for record in store.list_events("flush-error")] == ["FIRST", "SECOND"]


def test_closed_history_does_not_requeue_and_close_is_idempotent(tmp_path):
    store = HistoryStore(tmp_path / "history.sqlite3")
    history = HistoryService(EventBus(), store, "closed", origin="TEST")
    history._event("FIRST", "SYSTEM", "INFO", "Registro final")
    history.close()
    history.close()
    history._event("SECOND", "SYSTEM", "INFO", "Registro tardio")
    assert [record.kind for record in store.list_events("closed")] == ["FIRST"]
