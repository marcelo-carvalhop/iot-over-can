"""Classificação dos comandos, ocultação de segredos e autoria das associações."""
from __future__ import annotations

import pytest

from pico_tui.audit import (
    AuthorizationLedger,
    classify_command,
    command_action,
    log_event_for,
    make_action,
    redact_command,
)
from pico_tui.operators import Actor
from pico_tui.security import MUTATING_PREFIXES, is_mutating_command

ANA = Actor("ana", display="Ana Souza")
SYSTEM = Actor("marcelo", method="station")


class Clock:
    now = 100.0

    def __call__(self) -> float:
        return self.now


# ---------------------------------------------------------------------------
# Segredos
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("command", "shown"),
    [
        ("AUTH UNLOCK 0x1234567890ABCDEF", "AUTH UNLOCK ***"),
        ("NET WIFI PROVISION laboratorio minha-senha-wifi", "NET WIFI PROVISION laboratorio ***"),
        ("CMD TARGET=02.01 ACTION=CONFIG PASSWORD=segredo MODE=EVENTS", "CMD TARGET=02.01 ACTION=CONFIG PASSWORD=*** MODE=EVENTS"),
        ("WIRELESS BIND 2 AABBCCDD", "WIRELESS BIND 2 AABBCCDD"),
        ("  WIRELESS   BIND  2   AABBCCDD ", "WIRELESS BIND 2 AABBCCDD"),
    ],
)
def test_secrets_never_reach_the_record(command, shown):
    assert redact_command(command) == shown
    info = classify_command(command)
    assert "minha-senha-wifi" not in info.command + info.title
    assert "0x1234567890" not in info.command + info.title and "segredo" not in info.command


# ---------------------------------------------------------------------------
# Classificação
# ---------------------------------------------------------------------------


def test_bind_is_recorded_as_sensor_admission():
    info = classify_command("WIRELESS BIND 2 AABBCCDD")
    assert (info.kind, info.node, info.uuid, info.action) == ("SENSOR_BIND", 2, "AABBCCDD", "BIND")
    assert "AABBCCDD" in info.title and "Node 02" in info.title and "autorizada" in info.title
    assert "reassociação automática" in classify_command("WIRELESS BIND 2 AABBCCDD AUTO").title


def test_unbind_and_policy():
    unbind = classify_command("WIRELESS UNBIND 3 AABBCCDD")
    assert (unbind.kind, unbind.node, unbind.action) == ("SENSOR_UNBIND", 3, "UNBIND")
    policy = classify_command("WIRELESS POLICY 3 AABBCCDD MANUAL")
    assert policy.kind == "SENSOR_POLICY" and policy.title.endswith("MANUAL")


def test_gateway_commands_name_the_sensor_and_drop_the_transaction_id():
    info = classify_command("CMD TARGET=02.01 ACTION=CONFIG TX=ab12 MODE=STRUCTURAL RATE=250")
    assert (info.kind, info.sensor, info.node) == ("SENSOR_CONFIG", "02.01", 2)
    assert info.fields == {"MODE": "STRUCTURAL", "RATE": "250"} and "02.01" in info.title
    assert classify_command("CMD TARGET=02.01 ACTION=TELEMETRY TX=1 STATE=ON").title.startswith("Telemetria ligada")
    assert classify_command("CMD TARGET=02.01 ACTION=WIFI TX=1 STATE=OFF").title.startswith("Wi-Fi do módulo desligado")
    assert classify_command("CMD TARGET=02.01 ACTION=MISTERIO TX=1").title.startswith("Comando MISTERIO")


def test_can_administration_commands_are_read_by_what_the_firmware_executes():
    election = classify_command("22 00 FF 01")
    assert election.kind == "NETWORK_COMMAND" and "eleição" in election.title.lower()
    assert classify_command("22 0 ff 1").kind == "NETWORK_COMMAND"  # o sscanf do firmware aceita essa grafia
    off = classify_command("22 10 03 00")
    assert off.node == 3 and off.kind == "NETWORK_COMMAND"


def test_direct_sensor_commands():
    assert classify_command("SET GAIN 2").kind == "SENSOR_CONFIG"
    assert classify_command("DTC CLEAR").kind == "SENSOR_DTC_CLEAR"
    assert classify_command("FAULT POWER_CYCLE 2").kind == "FAULT_COMMAND"
    assert classify_command("TELEMETRY OFF").kind == "SENSOR_TELEMETRY"


def test_every_mutating_prefix_has_a_specific_title():
    """A tabela de auditoria precisa acompanhar a de comandos que exigem autorização."""

    for prefix in MUTATING_PREFIXES:
        sample = prefix + ("FF 01" if prefix.startswith("22 ") else "x" if prefix.endswith(" ") else "")
        assert is_mutating_command(sample), sample
        assert classify_command(sample).kind != "COMMAND", f"sem título específico para {prefix!r}"


def test_read_only_commands_are_not_mutating():
    for command in ("STATUS", "GET", "VERSION", "PING", "PROBE_STATUS", "22 20 FF 00", "NET WIFI STATUS", "TIME 1700000000000"):
        assert not is_mutating_command(command), command


# ---------------------------------------------------------------------------
# Eventos
# ---------------------------------------------------------------------------


def test_action_carries_who_what_and_outcome():
    info = classify_command("WIRELESS BIND 2 AABBCCDD")
    ok = command_action(info, ANA)
    assert (ok.kind, ok.outcome, ok.severity, ok.actor, ok.node_id) == ("SENSOR_BIND", "OK", "INFO", "ana", 2)
    assert ok.details["actor"] == {"id": "ana", "name": "ana", "display": "Ana Souza", "role": "operator",
                                   "method": "password", "verified": True, "station": ""}
    assert ok.details["command"] == "WIRELESS BIND 2 AABBCCDD"
    denied = command_action(info, None, outcome="DENIED", reason="Identifique-se")
    assert (denied.kind, denied.severity, denied.actor) == ("COMMAND_DENIED", "WARNING", None)
    assert denied.title.startswith("Recusado:") and denied.details["requested_kind"] == "SENSOR_BIND"
    failed = command_action(info, ANA, outcome="FAILED", reason="porta fechada")
    assert failed.kind == "COMMAND_FAILED" and failed.title.startswith("Falhou:")


def test_make_action_drops_empty_details():
    action = make_action("LOGIN", "Entrada", ANA, role="operator", reason="", attempted=None)
    assert "reason" not in action.details and "attempted" not in action.details
    assert action.details["role"] == "operator"
    assert make_action("X", "y", None).details == {"actor": {"id": None}}


def test_log_line_names_the_actor_or_says_nobody_was_identified():
    named = log_event_for(command_action(classify_command("RESET"), ANA), ANA)
    assert named.source == "AUDITORIA" and named.level == "INFO" and named.message.startswith("Ana Souza: ")
    anonymous = log_event_for(command_action(classify_command("RESET"), None, outcome="DENIED"), None)
    assert anonymous.level == "WARNING" and anonymous.message.startswith("Sem identificação: ")
    assert log_event_for(command_action(classify_command("RESET"), SYSTEM), SYSTEM).message.startswith("marcelo (sistema): ")


# ---------------------------------------------------------------------------
# Quem autorizou a associação
# ---------------------------------------------------------------------------


def test_bind_answers_are_credited_to_the_operator_who_asked():
    ledger = AuthorizationLedger(clock=Clock())
    ledger.remember(2, "AABBCCDD", "BIND", ANA)
    assert ledger.claim(2, "aabbccdd", "ASSOCIATING") == {"authorized_by": "ana", "authorized_action": "BIND"}
    assert ledger.claim(2, "AABBCCDD", "BOUND")["authorized_by"] == "ana"
    assert ledger.claim(2, "AABBCCDD", "ONLINE")["authorized_by"] == "ana"
    assert ledger.claim(2, "AABBCCDD", "STALE") is None  # depois da resposta final, o que acontece é da rede


def test_changes_the_network_makes_alone_have_no_author():
    ledger = AuthorizationLedger(clock=Clock())
    assert ledger.claim(2, "AABBCCDD", "STALE") is None
    ledger.remember(2, "AABBCCDD", "BIND", ANA)
    assert ledger.claim(2, "AABBCCDD", "STALE") is None and ledger.claim(2, "AABBCCDD", "LOST") is None
    assert ledger.claim(3, "AABBCCDD", "BOUND") is None  # outro módulo
    assert ledger.claim(2, "OUTRO", "BOUND") is None  # outro sensor


def test_answers_expire_and_unbind_is_credited_only_for_unbound():
    clock = Clock()
    ledger = AuthorizationLedger(ttl_s=60.0, clock=clock)
    ledger.remember(2, "AABBCCDD", "BIND", ANA)
    clock.now += 61
    assert ledger.claim(2, "AABBCCDD", "BOUND") is None
    ledger.remember(2, "AABBCCDD", "UNBIND", SYSTEM)
    assert ledger.claim(2, "AABBCCDD", "BOUND") is None
    assert ledger.claim(2, "AABBCCDD", "UNBOUND")["authorized_by"] == "sys:marcelo"
    assert ledger.claim(2, "AABBCCDD", "UNBOUND") is None  # já foi atribuído


def test_nothing_is_remembered_without_an_actor_or_for_policy():
    ledger = AuthorizationLedger(clock=Clock())
    ledger.remember(2, "AABBCCDD", "BIND", None)
    ledger.remember(2, "AABBCCDD", "POLICY", ANA)
    ledger.remember(None, "AABBCCDD", "BIND", ANA)
    assert ledger.claim(2, "AABBCCDD", "BOUND") is None


def test_uuid_value_reads_like_the_probe():
    """A Probe lê o identificador com strtoull (base 0): 0x, octal por zero à esquerda e decimal."""

    from pico_tui.audit import uuid_value

    full = 0x1122334455667788
    assert uuid_value("0x1122334455667788") == full
    assert uuid_value("0X1122334455667788") == full
    assert uuid_value("1234605616436508552") == full           # decimal
    assert uuid_value("0x0000000000001122") == uuid_value("0x1122") == 0x1122
    assert uuid_value("0755") == 0o755                         # zero à esquerda é octal, como no strtoull
    assert uuid_value("0x0") is None and uuid_value("") is None and uuid_value(None) is None
    assert uuid_value("0x") is None and uuid_value("abc") is None and uuid_value("1_000") is None


def test_ledger_credits_the_response_whatever_the_typed_form():
    """Digitado em decimal ou sem zeros, o comando ainda é creditado à resposta, que traz 0x e 16 dígitos."""

    from pico_tui.audit import AuthorizationLedger
    from pico_tui.operators import Actor

    actor = Actor(name="ana", role="operator", method="password", display="Ana", station="s", serial=1)
    for typed in ("0x1122", "4386", "0x0000000000001122", "0X1122"):
        ledger = AuthorizationLedger(clock=lambda: 0.0)
        ledger.remember(2, typed, "BIND", actor)
        stamp = ledger.claim(2, "0x0000000000001122", "BOUND")
        assert stamp == {"authorized_by": "ana", "authorized_action": "BIND"}, typed
    ledger = AuthorizationLedger(clock=lambda: 0.0)
    ledger.remember(2, "0x1122", "BIND", actor)
    assert ledger.claim(3, "0x0000000000001122", "BOUND") is None   # outro módulo
    assert ledger.claim(2, "0x0000000000001123", "BOUND") is None   # outro sensor


def test_refused_association_title_does_not_claim_it_was_authorized():
    from pico_tui.audit import classify_command, command_action
    from pico_tui.operators import Actor

    actor = Actor(name="visita", role="observer", method="password", display="Visita", station="s", serial=2)
    bind = classify_command("WIRELESS BIND 2 0x1122334455667788 AUTO")
    denied = command_action(bind, actor, outcome="DENIED", reason="observador")
    assert denied.title.startswith("Recusado: pedido de entrada do sensor") and "autorizada" not in denied.title
    unbind = classify_command("WIRELESS UNBIND 2 0x1122334455667788")
    failed = command_action(unbind, actor, outcome="FAILED")
    assert failed.title.startswith("Falhou: pedido para desassociar") and "desassociado" not in failed.title
    done = command_action(bind, actor)
    assert "autorizada no Node 02" in done.title          # o título da ação feita não muda
