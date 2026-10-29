"""Banco histórico SQLite separado do estado operacional dos microcontroladores.

Não grava telemetria contínua. Eventos têm instante de observação UTC, origem,
severidade e detalhes; indicadores CAN são reduzidos a uma amostra por janela.
Cada acesso utiliza uma conexão curta, permitindo execução em worker thread.
"""
from __future__ import annotations

import json
import sqlite3
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable
import csv
import hashlib
from collections import deque

SCHEMA = """
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS sessions (
    id TEXT PRIMARY KEY,
    started_epoch REAL NOT NULL,
    ended_epoch REAL,
    origin TEXT NOT NULL,
    port TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL REFERENCES sessions(id),
    epoch REAL NOT NULL,
    kind TEXT NOT NULL,
    category TEXT NOT NULL,
    severity TEXT NOT NULL,
    title TEXT NOT NULL,
    node_id INTEGER,
    sensor_id TEXT,
    details_json TEXT NOT NULL DEFAULT '{}'
);
CREATE INDEX IF NOT EXISTS idx_events_session_epoch ON events(session_id, epoch);
CREATE INDEX IF NOT EXISTS idx_events_session_category ON events(session_id, category, epoch);
CREATE TABLE IF NOT EXISTS bus_samples (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL REFERENCES sessions(id),
    epoch REAL NOT NULL,
    samples INTEGER NOT NULL,
    load_avg_pct REAL NOT NULL,
    peak_max_pct REAL NOT NULL,
    frames INTEGER NOT NULL,
    rx_errors INTEGER NOT NULL,
    tx_errors INTEGER NOT NULL,
    error_flags INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_bus_session_epoch ON bus_samples(session_id, epoch);
"""

CAPTURE_SCHEMA = """
CREATE TABLE IF NOT EXISTS captures (
 id TEXT PRIMARY KEY,
 session_id TEXT NOT NULL REFERENCES sessions(id),
 scope TEXT NOT NULL,
 target TEXT NOT NULL,
 trigger_epoch REAL NOT NULL,
 started_epoch REAL NOT NULL,
 end_target_epoch REAL NOT NULL,
 ended_epoch REAL,
 reason TEXT NOT NULL,
 state TEXT NOT NULL DEFAULT 'RECORDING',
 dropped_records INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_captures_session_period ON captures(session_id, started_epoch, end_target_epoch);
CREATE TABLE IF NOT EXISTS capture_events (
 capture_id TEXT NOT NULL REFERENCES captures(id),
 event_id INTEGER NOT NULL REFERENCES events(id),
 PRIMARY KEY(capture_id, event_id)
);
CREATE INDEX IF NOT EXISTS idx_capture_events_event ON capture_events(event_id);
CREATE TABLE IF NOT EXISTS capture_files (
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 capture_id TEXT NOT NULL REFERENCES captures(id),
 relative_path TEXT NOT NULL UNIQUE,
 kind TEXT NOT NULL,
 sensor_id TEXT,
 rows INTEGER NOT NULL,
 first_epoch REAL NOT NULL,
 last_epoch REAL NOT NULL,
 sha256 TEXT NOT NULL,
 bytes INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_capture_files_cap ON capture_files(capture_id);
"""


@dataclass(frozen=True)
class CaptureFile:
    id: int
    capture_id: str
    relative_path: str
    kind: str
    sensor_id: str | None
    rows: int
    first_epoch: float
    last_epoch: float
    sha256: str
    bytes: int


@dataclass(frozen=True)
class Session:
    id: str
    started_epoch: float
    ended_epoch: float | None
    origin: str
    port: str


@dataclass(frozen=True)
class HistoryEvent:
    id: int
    session_id: str
    epoch: float
    kind: str
    category: str
    severity: str
    title: str
    node_id: int | None
    sensor_id: str | None
    details: dict[str, Any]


class HistoryStore:
    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self._connect() as db:
            db.executescript(SCHEMA)
            db.executescript(CAPTURE_SCHEMA)

    def _connect(self) -> sqlite3.Connection:
        db = sqlite3.connect(self.path, timeout=3.0)
        db.execute("PRAGMA foreign_keys=ON")
        db.execute("PRAGMA busy_timeout=3000")
        return db

    def start_session(self, session_id: str, *, origin: str = "LIVE", port: str = "") -> float:
        started = time.time()
        with self._connect() as db:
            db.execute(
                "INSERT INTO sessions(id, started_epoch, origin, port) VALUES (?, ?, ?, ?)",
                (session_id, started, origin, port),
            )
        return started

    def finish_session(self, session_id: str, *, ended: float | None = None) -> None:
        with self._connect() as db:
            db.execute(
                "UPDATE sessions SET ended_epoch=? WHERE id=? AND ended_epoch IS NULL",
                (time.time() if ended is None else ended, session_id),
            )

    def write_batch(self, session_id: str, records: Iterable[tuple[str, dict[str, Any]]]) -> None:
        with self._connect() as db:
            for record_type, record in records:
                if record_type == "capture_extend":
                    db.execute("""UPDATE captures SET end_target_epoch=MAX(end_target_epoch, ?)
                                  WHERE id=?""", (record["end_target_epoch"], record["id"]))
                elif record_type == "capture_start":
                    db.execute(
                        """INSERT INTO captures(id,session_id,scope,target,trigger_epoch,started_epoch,
                           end_target_epoch,reason) VALUES (?,?,?,?,?,?,?,?)""",
                        (record["id"], session_id, record["scope"], record["target"],
                         record["trigger_epoch"], record["started_epoch"],
                         record["end_target_epoch"], record["reason"]),
                    )
                elif record_type == "event":
                    cursor = db.execute(
                        """INSERT INTO events
                        (session_id,epoch,kind,category,severity,title,node_id,sensor_id,details_json)
                        VALUES (?,?,?,?,?,?,?,?,?)""",
                        (session_id, record["epoch"], record["kind"], record["category"],
                         record["severity"], record["title"], record.get("node_id"),
                         record.get("sensor_id"), json.dumps(record.get("details", {}), ensure_ascii=False, default=str)),
                    )
                    for cid in record.get("capture_ids", ()):
                        db.execute("INSERT OR IGNORE INTO capture_events(capture_id,event_id) VALUES (?,?)",
                                   (cid, cursor.lastrowid))
                elif record_type == "bus":
                    db.execute(
                        """INSERT INTO bus_samples
                        (session_id,epoch,samples,load_avg_pct,peak_max_pct,frames,rx_errors,tx_errors,error_flags)
                        VALUES (?,?,?,?,?,?,?,?,?)""",
                        (session_id, record["epoch"], record["samples"], record["load_avg_pct"],
                         record["peak_max_pct"], record["frames"], record["rx_errors"],
                         record["tx_errors"], record["error_flags"]),
                    )

    def list_sessions(self, limit: int = 100) -> list[Session]:
        with self._connect() as db:
            rows = db.execute(
                "SELECT id,started_epoch,ended_epoch,origin,port FROM sessions ORDER BY started_epoch DESC LIMIT ?",
                (limit,),
            ).fetchall()
        return [Session(*row) for row in rows]

    def count_events(self, session_id: str, start: float | None = None, end: float | None = None) -> int:
        cond, params = self._range(session_id, start, end)
        with self._connect() as db:
            return int(db.execute(f"SELECT count(*) FROM events WHERE {cond}", params).fetchone()[0])

    @staticmethod
    def _range(session_id: str, start: float | None, end: float | None) -> tuple[str, tuple]:
        clauses = ["session_id=?"]
        params: list[Any] = [session_id]
        if start is not None:
            clauses.append("epoch>=?")
            params.append(start)
        if end is not None:
            clauses.append("epoch<=?")
            params.append(end)
        return " AND ".join(clauses), tuple(params)

    def list_events(self, session_id: str, *, start: float | None = None,
                    end: float | None = None, limit: int = 300) -> list[HistoryEvent]:
        cond, params = self._range(session_id, start, end)
        with self._connect() as db:
            rows = db.execute(
                f"""SELECT id,session_id,epoch,kind,category,severity,title,node_id,sensor_id,details_json
                    FROM events WHERE {cond} ORDER BY epoch,id LIMIT ?""",
                (*params, limit),
            ).fetchall()
        return [HistoryEvent(*row[:-1], json.loads(row[-1])) for row in rows]

    def adjacent_event(self, session_id: str, epoch: float, event_id: int, *, category: str,
                       forward: bool = True) -> HistoryEvent | None:
        """Evento seguinte (ou anterior) de uma faixa, na sessão inteira: navega pelas marcas do operador."""
        op, order = (">", "ASC") if forward else ("<", "DESC")
        with self._connect() as db:
            row = db.execute(
                f"""SELECT id,session_id,epoch,kind,category,severity,title,node_id,sensor_id,details_json
                    FROM events WHERE session_id=? AND category=? AND (epoch {op} ? OR (epoch=? AND id {op} ?))
                    ORDER BY epoch {order}, id {order} LIMIT 1""",
                (session_id, category, epoch, epoch, event_id),
            ).fetchone()
        return HistoryEvent(*row[:-1], json.loads(row[-1])) if row else None

    def nearest_bus(self, session_id: str, epoch: float, max_age_s: float = 120.0) -> dict | None:
        with self._connect() as db:
            row = db.execute(
                """SELECT epoch,samples,load_avg_pct,peak_max_pct,frames,rx_errors,tx_errors,error_flags
                   FROM bus_samples WHERE session_id=? AND epoch<=? ORDER BY epoch DESC LIMIT 1""",
                (session_id, epoch),
            ).fetchone()
        if row is None or epoch - row[0] > max_age_s:
            return None
        return dict(zip(("epoch", "samples", "load_avg_pct", "peak_max_pct", "frames",
                         "rx_errors", "tx_errors", "error_flags"), row))

    def upsert_capture_file(self, record: dict[str, Any]) -> None:
        with self._connect() as db:
            db.execute("""INSERT INTO capture_files
                (capture_id,relative_path,kind,sensor_id,rows,first_epoch,last_epoch,sha256,bytes)
                VALUES (?,?,?,?,?,?,?,?,?)
                ON CONFLICT(relative_path) DO UPDATE SET
                  rows=excluded.rows,first_epoch=excluded.first_epoch,
                  last_epoch=excluded.last_epoch,sha256=excluded.sha256,bytes=excluded.bytes""",
                (record['capture_id'], record['relative_path'], record['kind'], record['sensor_id'],
                 record['rows'], record['first_epoch'], record['last_epoch'], record['sha256'], record['bytes']))

    def finish_capture(self, cid: str, *, ended_epoch: float,
                       dropped_records: int, state: str) -> None:
        with self._connect() as db:
            db.execute("""UPDATE captures SET ended_epoch=?, dropped_records=?, state=?
                          WHERE id=? AND ended_epoch IS NULL""",
                       (ended_epoch, dropped_records, state, cid))

    def capture_for_event(self, event_id: int) -> list[dict[str, Any]]:
        with self._connect() as db:
            rows = db.execute("""SELECT c.id,c.scope,c.target,c.started_epoch,c.end_target_epoch,
                                      c.ended_epoch,c.state,c.dropped_records
                                  FROM captures c JOIN capture_events ce ON ce.capture_id=c.id
                                  WHERE ce.event_id=? ORDER BY c.trigger_epoch""", (event_id,)).fetchall()
        keys = ('id','scope','target','started_epoch','end_target_epoch','ended_epoch','state','dropped_records')
        return [dict(zip(keys, row)) for row in rows]

    def capture_intervals(self, session_id: str, start: float, end: float,
                          limit: int = 150) -> list[dict[str, Any]]:
        with self._connect() as db:
            rows = db.execute("""SELECT id,started_epoch,COALESCE(ended_epoch,end_target_epoch),state
                                 FROM captures WHERE session_id=? AND started_epoch<=?
                                 AND COALESCE(ended_epoch,end_target_epoch)>=? ORDER BY started_epoch LIMIT ?""",
                              (session_id,end,start,limit)).fetchall()
        return [dict(zip(('id','start','end','state'), row)) for row in rows]

    def files_for_event(self, event_id: int, limit: int = 30) -> list[CaptureFile]:
        with self._connect() as db:
            rows = db.execute("""SELECT DISTINCT f.id,f.capture_id,f.relative_path,f.kind,f.sensor_id,
                                      f.rows,f.first_epoch,f.last_epoch,f.sha256,f.bytes
                                 FROM capture_files f JOIN capture_events ce ON ce.capture_id=f.capture_id
                                 WHERE ce.event_id=? ORDER BY f.first_epoch DESC,f.id DESC LIMIT ?""",
                              (event_id,limit)).fetchall()
        return [CaptureFile(*row) for row in rows]

    def preview_csv(self, file: CaptureFile, *, max_rows: int = 3) -> tuple[str, list[dict[str, str]]]:
        """Retorna status e últimas amostras, sem carregar o CSV integral em memória."""
        root = self.path.parent.resolve()
        path = (root / file.relative_path).resolve()
        if not path.is_relative_to(root):
            return 'CAMINHO_INVALIDO', []
        if not path.is_file():
            return 'AUSENTE', []
        if hashlib.sha256(path.read_bytes()).hexdigest() != file.sha256:
            return 'INTEGRIDADE_INVALIDA', []
        tail: deque[dict[str,str]] = deque(maxlen=max_rows)
        with path.open('r',encoding='utf-8', newline='') as handle:
            for row in csv.DictReader(handle):
                tail.append(row)
        return 'OK', list(tail)
