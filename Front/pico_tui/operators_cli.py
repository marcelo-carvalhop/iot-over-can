"""Cadastro de operadores e consulta da trilha de auditoria (``iot-over-can-operators``).

Exemplos::

    iot-over-can-operators add ana --display "Ana Souza"
    iot-over-can-operators add visita --role observer
    iot-over-can-operators list
    iot-over-can-operators passwd ana
    iot-over-can-operators disable visita
    iot-over-can-operators audit --operator ana --since 24h
    iot-over-can-operators audit --verify

A senha é sempre pedida no terminal, sem eco (ou lida da entrada padrão com
``--password-stdin``, para scripts). Nunca é aceita como argumento: ficaria no
histórico do shell e na lista de processos.
"""
from __future__ import annotations

import argparse
import getpass
import json
import re
import sys
import time
from collections.abc import Sequence
from datetime import datetime
from pathlib import Path

from pico_tui.history.storage import HistoryStore
from pico_tui.operators import (
    ROLE_LABELS,
    ROLE_OPERATOR,
    ROLES,
    OperatorError,
    OperatorRegistry,
    default_registry_path,
    validate_password,
)


def _read_password(args: argparse.Namespace, name: str) -> str:
    if args.password_stdin:
        password = sys.stdin.readline().rstrip("\r\n")
    else:
        password = getpass.getpass(f"Senha de {name}: ")
        if password != getpass.getpass("Repita a senha: "):
            raise OperatorError("As senhas não conferem. Nada foi alterado.")
    validate_password(password, name)
    return password


def _registry(args: argparse.Namespace) -> OperatorRegistry:
    return OperatorRegistry(args.file or default_registry_path())


def _cmd_list(args: argparse.Namespace) -> int:
    registry = _registry(args)
    if registry.error:
        print(f"Cadastro ilegível: {registry.error}", file=sys.stderr)
        return 1
    if not registry.records():
        print(f"Nenhum operador cadastrado em {registry.path}.")
        print("Sem cadastro, a TUI identifica o usuário do sistema. Para identificar por senha: add NOME")
        return 0
    print(f"{'NOME':<20}{'PERFIL':<13}{'ESTADO':<10}{'CRIADO':<22}NOME COMPLETO")
    for record in registry.records():
        print(f"{record.name:<20}{ROLE_LABELS[record.role]:<13}{'ativo' if record.enabled else 'inativo':<10}"
              f"{record.created or '-':<22}{record.display}")
    return 0


def _cmd_add(args: argparse.Namespace) -> int:
    registry = _registry(args)
    password = _read_password(args, args.name)
    record = registry.add(args.name, password, role=args.role, display=args.display or "")
    print(f"Operador {record.name} cadastrado ({ROLE_LABELS[record.role]}). Cadastro: {registry.path}")
    return 0


def _cmd_passwd(args: argparse.Namespace) -> int:
    registry = _registry(args)
    registry.reload()
    if registry.get(args.name) is None:
        raise OperatorError(f"Operador não encontrado: {args.name}")
    registry.set_password(args.name, _read_password(args, args.name))
    print(f"Senha de {args.name} trocada. Sessões abertas dele na TUI serão encerradas.")
    return 0


def _cmd_enable(args: argparse.Namespace) -> int:
    _registry(args).set_enabled(args.name, True)
    print(f"Operador {args.name} ativado.")
    return 0


def _cmd_disable(args: argparse.Namespace) -> int:
    _registry(args).set_enabled(args.name, False)
    print(f"Operador {args.name} desativado. A sessão dele na TUI, se houver, será encerrada.")
    return 0


def _cmd_role(args: argparse.Namespace) -> int:
    _registry(args).set_role(args.name, args.role)
    print(f"Operador {args.name}: perfil {ROLE_LABELS[args.role]}.")
    return 0


def _cmd_remove(args: argparse.Namespace) -> int:
    if not args.yes:
        answer = input(f"Remover {args.name} do cadastro? O histórico dele continua no banco. [s/N] ")
        if answer.strip().lower() not in {"s", "sim", "y", "yes"}:
            print("Nada foi alterado.")
            return 1
    _registry(args).remove(args.name)
    print(f"Operador {args.name} removido do cadastro.")
    return 0


def _cmd_check(args: argparse.Namespace) -> int:
    registry = _registry(args)
    if not registry.exists:
        print(f"Sem cadastro em {registry.path}: a TUI identifica o usuário do sistema.")
        return 0
    if registry.error:
        print(f"PROBLEMA: {registry.error}", file=sys.stderr)
        return 1
    active = registry.active_count()
    print(f"Cadastro válido: {len(registry.records())} operador(es), {active} ativo(s). Arquivo: {registry.path}")
    if not active:
        print("AVISO: nenhum operador ativo; a TUI vai bloquear os comandos que alteram a rede.", file=sys.stderr)
    return 0


_SINCE = re.compile(r"^(\d+)([hd])$")


def _parse_since(text: str | None) -> float | None:
    if not text:
        return None
    match = _SINCE.match(text.strip().lower())
    if match:
        seconds = int(match.group(1)) * (3600 if match.group(2) == "h" else 86400)
        return time.time() - seconds
    try:
        return datetime.fromisoformat(text).timestamp()
    except ValueError as exc:
        raise OperatorError("Use --since 24h, 7d ou uma data como 2026-11-12.") from exc


def _cmd_audit(args: argparse.Namespace) -> int:
    db_path = Path(args.db)
    if not db_path.exists():
        print(f"Nenhum histórico em {db_path}. Rode a TUI nesta pasta ou use --db.", file=sys.stderr)
        return 1
    store = HistoryStore(db_path)
    status = 0
    if args.verify:
        checked, bad = store.verify_audit_chain()
        if bad is None:
            print(f"Cadeia de auditoria íntegra: {checked} registro(s) conferido(s).")
        else:
            print(f"ADULTERAÇÃO: o registro {bad} não confere com a cadeia ({checked} anteriores íntegros).",
                  file=sys.stderr)
            status = 2
        if not (args.operator or args.since):
            return status
    events = store.list_audit(actor=args.operator, since=_parse_since(args.since), limit=args.limit)
    for event in reversed(events):
        stamp = datetime.fromtimestamp(event.epoch).astimezone().strftime("%Y-%m-%d %H:%M:%S")
        outcome = str(event.details.get("outcome", "OK"))
        if args.json:
            print(json.dumps({"time": stamp, "actor": event.actor, "kind": event.kind, "outcome": outcome,
                              "title": event.title, "session": event.session_id, "details": event.details},
                             ensure_ascii=False))
        else:
            flag = "" if outcome == "OK" else f" [{outcome}]"
            print(f"{stamp}  {event.actor or '-':<16} {event.kind:<18} {event.title}{flag}")
    if not events:
        print("Nenhuma ação registrada com esses filtros.")
    return status


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="iot-over-can-operators",
        description="Cadastro de operadores da TUI e consulta da trilha de auditoria.",
    )
    parser.add_argument("--file", default=None, metavar="ARQUIVO",
                        help="Cadastro de operadores (padrão: ~/.config/iot-over-can/operators.json)")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="Lista os operadores cadastrados").set_defaults(run=_cmd_list)

    add = sub.add_parser("add", help="Cadastra um operador (a senha é pedida no terminal)")
    add.add_argument("name", help="Nome de acesso: minúsculas, números, ponto, hífen ou sublinhado")
    add.add_argument("--role", choices=ROLES, default=ROLE_OPERATOR,
                     help="operator altera a rede; observer só acompanha (padrão: operator)")
    add.add_argument("--display", default="", help="Nome completo, mostrado na TUI e no histórico")
    add.add_argument("--password-stdin", action="store_true", help="Lê a senha da entrada padrão (scripts)")
    add.set_defaults(run=_cmd_add)

    passwd = sub.add_parser("passwd", help="Troca a senha (encerra as sessões abertas do operador)")
    passwd.add_argument("name")
    passwd.add_argument("--password-stdin", action="store_true")
    passwd.set_defaults(run=_cmd_passwd)

    for name, helper, run in (("enable", "Reativa um operador", _cmd_enable),
                              ("disable", "Desativa um operador sem apagar o cadastro", _cmd_disable)):
        item = sub.add_parser(name, help=helper)
        item.add_argument("name")
        item.set_defaults(run=run)

    role = sub.add_parser("role", help="Troca o perfil de um operador")
    role.add_argument("name")
    role.add_argument("role", choices=ROLES)
    role.set_defaults(run=_cmd_role)

    remove = sub.add_parser("remove", help="Remove um operador do cadastro (o histórico dele permanece)")
    remove.add_argument("name")
    remove.add_argument("--yes", action="store_true", help="Não pede confirmação")
    remove.set_defaults(run=_cmd_remove)

    sub.add_parser("check", help="Confere o arquivo de cadastro (formato e permissões)").set_defaults(run=_cmd_check)

    audit = sub.add_parser("audit", help="Mostra as ações dos operadores gravadas no histórico")
    audit.add_argument("--db", default="logs/historico.sqlite3", metavar="ARQUIVO",
                       help="Banco do histórico (padrão: logs/historico.sqlite3, a partir da pasta atual)")
    audit.add_argument("--operator", default=None, metavar="NOME", help="Só as ações deste operador (ex.: ana, sys:marcelo)")
    audit.add_argument("--since", default=None, metavar="PERÍODO", help="24h, 7d ou uma data (2026-11-12)")
    audit.add_argument("--limit", type=int, default=200, help="Máximo de registros (padrão 200)")
    audit.add_argument("--verify", action="store_true", help="Confere a cadeia de hashes e aponta registros adulterados")
    audit.add_argument("--json", action="store_true", help="Uma linha JSON por registro")
    audit.set_defaults(run=_cmd_audit)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return int(args.run(args))
    except OperatorError as exc:
        print(f"Erro: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nInterrompido. Nada foi alterado.", file=sys.stderr)
        return 130


def run() -> None:
    raise SystemExit(main())


if __name__ == "__main__":
    run()
