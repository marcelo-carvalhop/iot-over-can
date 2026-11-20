"""iot-over-can-operators: cadastro pela linha de comando e consulta da auditoria."""
from __future__ import annotations

import io
import json
import os
import sqlite3
import stat

import pytest

from pico_tui.history.storage import HistoryStore
from pico_tui.operators import OperatorRegistry
from pico_tui.operators_cli import main

PASSWORD = "senha-da-ana-1"


@pytest.fixture
def run(tmp_path, monkeypatch, capsys):
    registry = tmp_path / "operators.json"

    def call(*argv: str, stdin: str | None = None) -> tuple[int, str, str]:
        if stdin is not None:
            monkeypatch.setattr("sys.stdin", io.StringIO(stdin))
        code = main(["--file", str(registry), *argv])
        out = capsys.readouterr()
        return code, out.out, out.err

    call.registry = registry  # type: ignore[attr-defined]
    return call


def test_add_list_and_check(run):
    assert run("list")[1].startswith("Nenhum operador cadastrado")
    code, out, _ = run("add", "ana", "--display", "Ana Souza", "--password-stdin", stdin=PASSWORD + "\n")
    assert code == 0 and "Operador ana cadastrado (operador)" in out
    run("add", "visita", "--role", "observer", "--password-stdin", stdin="senha-visita-1\n")
    _, listing, _ = run("list")
    assert "Ana Souza" in listing and "observador" in listing and "ativo" in listing
    assert PASSWORD not in listing and PASSWORD not in run.registry.read_text()
    if os.name == "posix":
        assert stat.S_IMODE(run.registry.stat().st_mode) == 0o600
    code, out, _ = run("check")
    assert code == 0 and "2 operador(es), 2 ativo(s)" in out


def test_invalid_input_is_refused_without_touching_the_registry(run):
    assert run("add", "ana", "--password-stdin", stdin="curta\n")[0] == 1
    assert run("add", "Ana Souza", "--password-stdin", stdin=PASSWORD + "\n")[0] == 1
    assert not run.registry.exists()
    run("add", "ana", "--password-stdin", stdin=PASSWORD + "\n")
    code, _, err = run("add", "ana", "--password-stdin", stdin=PASSWORD + "\n")
    assert code == 1 and "já cadastrado" in err
    assert run("disable", "ninguem")[0] == 1


def test_password_change_disable_role_and_remove(run):
    run("add", "ana", "--password-stdin", stdin=PASSWORD + "\n")
    assert run("passwd", "ana", "--password-stdin", stdin="senha-nova-valida\n")[0] == 0
    registry = OperatorRegistry(run.registry)
    assert registry.verify("ana", "senha-nova-valida") and not registry.verify("ana", PASSWORD)
    assert run("disable", "ana")[0] == 0 and "inativo" in run("list")[1]
    assert run("enable", "ana")[0] == 0 and run("role", "ana", "observer")[0] == 0
    assert "observador" in run("list")[1]
    assert run("remove", "ana", "--yes")[0] == 0 and run("list")[1].startswith("Nenhum operador cadastrado")


def test_check_reports_a_broken_registry(run):
    run.registry.write_text("{")
    run.registry.chmod(0o600)
    code, _, err = run("check")
    assert code == 1 and "PROBLEMA" in err
    assert run("add", "ana", "--password-stdin", stdin=PASSWORD + "\n")[0] == 1  # não sobrescreve o que não leu


def seed_history(path, *, tamper: bool = False) -> None:
    store = HistoryStore(path)
    store.start_session("s1")
    base = {"severity": "INFO", "node_id": 2, "sensor_id": None}
    store.write_batch("s1", [
        ("event", {**base, "epoch": 100.0, "kind": "LOGIN", "category": "AUDIT", "title": "Entrada de Ana",
                   "details": {"outcome": "OK"}, "actor": "ana"}),
        ("event", {**base, "epoch": 200.0, "kind": "SENSOR_BIND", "category": "AUDIT",
                   "title": "Entrada do sensor X autorizada no Node 02", "details": {"outcome": "OK"}, "actor": "ana"}),
        ("event", {**base, "epoch": 300.0, "kind": "COMMAND_DENIED", "category": "AUDIT", "title": "Recusado: RESET",
                   "details": {"outcome": "DENIED"}, "actor": "visita"}),
    ])
    if tamper:
        with sqlite3.connect(path) as db:
            db.execute("UPDATE events SET title='Entrada de outra pessoa' WHERE kind='LOGIN'")


def test_audit_lists_actions_with_author_and_outcome(run, tmp_path, capsys):
    db = tmp_path / "historico.sqlite3"
    seed_history(db)
    code, out, _ = run("audit", "--db", str(db))
    lines = out.strip().splitlines()
    assert code == 0 and len(lines) == 3
    assert "ana" in lines[0] and "LOGIN" in lines[0]  # do mais antigo ao mais recente
    assert "visita" in lines[2] and lines[2].endswith("[DENIED]")
    only_ana = run("audit", "--db", str(db), "--operator", "ana")[1].strip().splitlines()
    assert len(only_ana) == 2 and all("visita" not in line for line in only_ana)
    as_json = [json.loads(line) for line in run("audit", "--db", str(db), "--json")[1].strip().splitlines()]
    assert [item["actor"] for item in as_json] == ["ana", "ana", "visita"]
    assert run("audit", "--db", str(db), "--since", "1h")[1].startswith("Nenhuma ação")  # tudo é de 1970
    assert run("audit", "--db", str(db), "--since", "ontem")[0] == 1


def test_audit_verify_detects_edited_records(run, tmp_path):
    clean, edited = tmp_path / "limpo.sqlite3", tmp_path / "editado.sqlite3"
    seed_history(clean)
    seed_history(edited, tamper=True)
    code, out, _ = run("audit", "--db", str(clean), "--verify")
    assert code == 0 and "íntegra: 3 registro(s)" in out
    code, _, err = run("audit", "--db", str(edited), "--verify")
    assert code == 2 and "ADULTERAÇÃO" in err
    assert run("audit", "--db", str(tmp_path / "nao-existe.sqlite3"))[0] == 1


def write_anchor(directory, store, *, name="session_s1.jsonl", final=True):
    count, head = store.audit_head()
    record = {"type": "audit_head", "session_id": "s1", "count": count, "head": head, "final": final}
    (directory / name).write_text(json.dumps(record) + "\n", encoding="utf-8")
    return count, head


def test_audit_anchor_detects_truncated_tail(run, tmp_path):
    """O corte dos últimos registros, que a cadeia sozinha não revela, aparece com o hash final anotado."""

    db = tmp_path / "historico.sqlite3"
    seed_history(db)
    store = HistoryStore(db)
    count, head = write_anchor(tmp_path, store)
    assert count == 3 and len(head) == 64

    code, out, _ = run("audit", "--db", str(db), "--verify")
    assert code == 0 and "íntegra: 3 registro(s)" in out and "Âncoras de sessão: 1 arquivo(s)" in out

    with sqlite3.connect(db) as conn:  # apaga os dois últimos registros de auditoria
        conn.execute("DELETE FROM events WHERE kind IN ('SENSOR_BIND', 'COMMAND_DENIED')")
    code, out, err = run("audit", "--db", str(db), "--verify")
    assert "íntegra: 1 registro(s)" in out  # a cadeia, sozinha, continua íntegra
    assert code == 2 and "CORTE OU SUBSTITUIÇÃO" in err and "session_s1.jsonl" in err


def test_audit_anchor_absent_is_reported_not_failed(run, tmp_path):
    db = tmp_path / "historico.sqlite3"
    seed_history(db)
    code, out, _ = run("audit", "--db", str(db), "--verify")
    assert code == 0 and "Sem âncoras de sessão" in out


def test_audit_anchor_position_mismatch(run, tmp_path):
    db = tmp_path / "historico.sqlite3"
    seed_history(db)
    store = HistoryStore(db)
    _, head = store.audit_head()
    record = {"type": "audit_head", "count": 5, "head": head}  # o hash existe, mas na posição 3
    (tmp_path / "session_x.jsonl").write_text(json.dumps(record) + "\n", encoding="utf-8")
    code, _, err = run("audit", "--db", str(db), "--verify")
    assert code == 2 and "posição 3" in err


def test_audit_anchors_option_reads_another_folder(run, tmp_path):
    db = tmp_path / "copia.sqlite3"
    seed_history(db)
    logs = tmp_path / "logs"
    logs.mkdir()
    write_anchor(logs, HistoryStore(db))
    code, out, _ = run("audit", "--db", str(db), "--verify", "--anchors", str(logs))
    assert code == 0 and "Âncoras de sessão: 1" in out


def test_check_warning_depends_on_why_nothing_is_active(run):
    assert run("add", "ana", "--password-stdin", stdin=PASSWORD + "\n")[0] == 0
    assert run("disable", "ana")[0] == 0
    code, out, err = run("check")
    assert code == 0 and "1 operador(es), 0 ativo(s)" in out
    assert "todos os operadores estão desativados" in err and "vai bloquear" in err

    assert run("remove", "ana", "--yes")[0] == 0
    code, out, err = run("check")
    # Cadastro vazio no modo padrão não bloqueia: a TUI volta a registrar a conta do sistema.
    assert code == 0 and "cadastro está vazio" in err and "não bloqueia nada" in err
    assert "required" in err and "vai bloquear os comandos" not in err
