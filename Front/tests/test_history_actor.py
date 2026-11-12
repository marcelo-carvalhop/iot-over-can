"""Autoria no histórico: migração do banco, cadeia de hashes e registro das ações do operador."""
from __future__ import annotations

import asyncio
import sqlite3

import pytest

from pico_tui.audit import AuthorizationLedger
from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import BusStatsReceived, OperatorAction, WirelessAssociationReceived
from pico_tui.history import service as service_module
from pico_tui.history.captures import decide_scope
from pico_tui.history.service import HistoryService
from pico_tui.history.storage import HistoryStore
from pico_tui.operators import Actor

OLD_SCHEMA = """
CREATE TABLE sessions (id TEXT PRIMARY KEY, started_epoch REAL NOT NULL, ended_epoch REAL,
                       origin TEXT NOT NULL, port TEXT NOT NULL DEFAULT '');
CREATE TABLE events (id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL REFERENCES sessions(id),
                     epoch REAL NOT NULL, kind TEXT NOT NULL, category TEXT NOT NULL, severity TEXT NOT NULL,
                     title TEXT NOT NULL, node_id INTEGER, sensor_id TEXT, details_json TEXT NOT NULL DEFAULT '{}');
CREATE TABLE bus_samples (id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL REFERENCES sessions(id),
                          epoch REAL NOT NULL, samples INTEGER NOT NULL, load_avg_pct REAL NOT NULL,
                          peak_max_pct REAL NOT NULL, frames INTEGER NOT NULL, rx_errors INTEGER NOT NULL,
                          tx_errors INTEGER NOT NULL, error_flags INTEGER NOT NULL);
"""


def audit_record(index: int, *, actor: str | None = "ana", category: str = "AUDIT", epoch: float | None = None) -> tuple:
    return ("event", {"epoch": 1000.0 + index if epoch is None else epoch, "kind": "SENSOR_BIND", "category": category,
                      "severity": "INFO", "title": f"registro {index}", "node_id": 2, "sensor_id": None,
                      "details": {"n": index}, "actor": actor})


@pytest.fixture
def store(tmp_path) -> HistoryStore:
    db = HistoryStore(tmp_path / "history.sqlite3")
    db.start_session("s1", station="ana@bancada")
    return db


# ---------------------------------------------------------------------------
# Banco
# ---------------------------------------------------------------------------


def test_database_created_before_operator_identification_is_migrated_in_place(tmp_path):
    path = tmp_path / "old.sqlite3"
    with sqlite3.connect(path) as db:
        db.executescript(OLD_SCHEMA)
        db.execute("INSERT INTO sessions(id,started_epoch,origin,port) VALUES ('velha',1.0,'LIVE','/dev/ttyACM0')")
        db.execute("INSERT INTO events(session_id,epoch,kind,category,severity,title,details_json) "
                   "VALUES ('velha',2.0,'DTC','SENSOR','WARNING','antigo','{\"a\": 1}')")
    migrated = HistoryStore(path)
    old = migrated.list_events("velha")
    assert [(e.title, e.actor, e.details) for e in old] == [("antigo", None, {"a": 1})]
    assert migrated.list_sessions()[0].station == ""
    migrated.start_session("nova", station="ana@bancada")
    migrated.write_batch("nova", [audit_record(0)])
    assert migrated.list_events("nova")[0].actor == "ana"
    assert migrated.verify_audit_chain() == (1, None)
    HistoryStore(path)  # abrir de novo não migra duas vezes nem falha


def test_actor_and_station_are_stored_and_listed(store):
    store.write_batch("s1", [audit_record(0, actor="ana"), audit_record(1, actor="sys:marcelo"),
                             audit_record(2, actor=None, category="SENSOR")])
    assert [e.actor for e in store.list_events("s1")] == ["ana", "sys:marcelo", None]
    assert store.list_sessions()[0].station == "ana@bancada"
    assert [e.title for e in store.list_audit(actor="ana")] == ["registro 0"]
    assert [e.title for e in store.list_audit()] == ["registro 1", "registro 0"]  # mais recente primeiro
    assert [e.title for e in store.list_audit(since=1001.0)] == ["registro 1"]
    assert store.list_audit(session_id="outra") == []


def test_audit_chain_is_intact_and_only_audit_events_join_it(store):
    store.write_batch("s1", [audit_record(0), audit_record(1, category="SENSOR", actor=None), audit_record(2)])
    store.write_batch("s1", [audit_record(3)])
    assert store.verify_audit_chain() == (3, None)
    with sqlite3.connect(store.path) as db:
        chains = dict(db.execute("SELECT category, count(chain) FROM events GROUP BY category").fetchall())
    assert chains == {"AUDIT": 3, "SENSOR": 0}


def test_editing_a_record_breaks_the_chain_at_that_record(store):
    store.write_batch("s1", [audit_record(0), audit_record(1), audit_record(2)])
    target = store.list_events("s1")[1]
    for column, value in (("title", "outra coisa"), ("actor", "bia"), ("details_json", '{"n": 99}'), ("epoch", 5.0)):
        with sqlite3.connect(store.path) as db:
            original = db.execute(f"SELECT {column} FROM events WHERE id=?", (target.id,)).fetchone()[0]
            db.execute(f"UPDATE events SET {column}=? WHERE id=?", (value, target.id))
        assert store.verify_audit_chain() == (1, target.id), column
        with sqlite3.connect(store.path) as db:
            db.execute(f"UPDATE events SET {column}=? WHERE id=?", (original, target.id))
    assert store.verify_audit_chain() == (3, None)


def test_deleting_a_record_in_the_middle_is_detected(store):
    store.write_batch("s1", [audit_record(0), audit_record(1), audit_record(2)])
    _, middle, last = store.list_events("s1")
    with sqlite3.connect(store.path) as db:
        db.execute("DELETE FROM events WHERE id=?", (middle.id,))
    assert store.verify_audit_chain() == (1, last.id)


def test_two_processes_writing_the_same_database_keep_one_chain(tmp_path):
    path = tmp_path / "shared.sqlite3"
    first, second = HistoryStore(path), HistoryStore(path)
    first.start_session("a")
    second.start_session("b")
    for round_ in range(3):
        first.write_batch("a", [audit_record(round_ * 2)])
        second.write_batch("b", [audit_record(round_ * 2 + 1)])
    assert first.verify_audit_chain() == (6, None)


def test_empty_database_verifies(store):
    assert store.verify_audit_chain() == (0, None)


# ---------------------------------------------------------------------------
# Serviço
# ---------------------------------------------------------------------------


def make_service(tmp_path, **kwargs) -> tuple[EventBus, HistoryStore, HistoryService]:
    bus, db = EventBus(), HistoryStore(tmp_path / "service.sqlite3")
    return bus, db, HistoryService(bus, db, "sessao", **kwargs)


def test_operator_actions_are_recorded_with_their_author(tmp_path):
    bus, db, history = make_service(tmp_path, station="ana@bancada")
    ana = Actor("ana", display="Ana Souza")

    async def publish():
        await bus.publish(BusStatsReceived(1000, 1000, 12, 1200, 1.2, 3.1))  # cria bus_at_observation para os demais
        await bus.publish(OperatorAction("SENSOR_BIND", "Entrada do sensor X autorizada no Node 02", actor="ana",
                                         node_id=2, details={"actor": ana.record(), "command": "WIRELESS BIND 2 X"}))
        await bus.publish(OperatorAction("COMMAND_DENIED", "Recusado: RESET", outcome="DENIED", severity="WARNING",
                                         details={"actor": {"id": None}}))
    asyncio.run(publish())
    history.close()
    events = {e.kind: e for e in db.list_events("sessao")}
    bind = events["SENSOR_BIND"]
    assert (bind.category, bind.actor, bind.node_id, bind.severity) == ("AUDIT", "ana", 2, "INFO")
    assert bind.details["outcome"] == "OK" and bind.details["command"] == "WIRELESS BIND 2 X"
    assert "bus_at_observation" not in bind.details  # registro enxuto: a auditoria não carrega fotografia da rede
    denied = events["COMMAND_DENIED"]
    assert denied.actor is None and denied.severity == "WARNING" and denied.details["outcome"] == "DENIED"
    assert db.list_sessions()[0].station == "ana@bancada"
    assert db.verify_audit_chain() == (2, None)


def test_association_events_are_credited_only_when_an_operator_asked_for_them(tmp_path):
    bus, db, history = make_service(tmp_path)
    history.ledger = AuthorizationLedger()
    history.ledger.remember(2, "AABBCCDD", "BIND", Actor("ana"))

    async def publish():
        await bus.publish(WirelessAssociationReceived(2, 1, "AABBCCDD", "VIBRATION", "BOUND", -44))
        await bus.publish(WirelessAssociationReceived(2, 1, "AABBCCDD", "VIBRATION", "STALE", -80))  # a rede, sozinha
        await bus.publish(WirelessAssociationReceived(3, 1, "11223344", "VIBRATION", "BOUND", -50))  # ninguém pediu
    asyncio.run(publish())
    history.close()
    bound, stale, other = db.list_events("sessao")
    assert bound.actor == "ana" and bound.details["authorized_by"] == "ana" and bound.details["authorized_action"] == "BIND"
    assert stale.actor is None and "authorized_by" not in stale.details
    assert other.actor is None


def test_audit_records_are_the_last_to_be_dropped_when_the_queue_overflows(tmp_path, monkeypatch):
    monkeypatch.setattr(service_module, "MAX_QUEUE", 4)
    bus, db, history = make_service(tmp_path)

    async def publish():
        await bus.publish(OperatorAction("LOGIN", "Entrada de ana", actor="ana", details={"actor": {"id": "ana"}}))
        for index in range(20):  # inundação de eventos da rede
            history._event("LINK_STATE", "WIRELESS", "INFO", f"enlace {index}")
    asyncio.run(publish())
    history.close()
    kinds = [e.kind for e in db.list_events("sessao")]
    assert "LOGIN" in kinds and "HISTORY_QUEUE_OVERFLOW" in kinds
    assert db.verify_audit_chain() == (1, None)


def test_audit_actions_are_context_for_captures_never_their_trigger():
    assert decide_scope({"kind": "FAULT_STARTED", "category": "AUDIT", "severity": "INFO", "node_id": 2}) is None
    assert decide_scope({"kind": "FAULT_STARTED", "category": "NETWORK", "severity": "INFO", "node_id": 2}) is not None
