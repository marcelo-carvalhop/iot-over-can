"""Cadastro de operadores, senhas, sessão com expiração e modos de identificação."""
from __future__ import annotations

import json
import os
import stat
import unicodedata

import pytest

from pico_tui.operators import (
    ROLE_OBSERVER,
    ROLE_OPERATOR,
    Actor,
    OperatorError,
    OperatorRegistry,
    OperatorSession,
    hash_password,
    station_actor,
    verify_password,
)

FAST = (16, 1, 1)  # scrypt barato: os testes não medem a força do hash
STATION = station_actor("marcelo", "bancada")


class Clock:
    def __init__(self) -> None:
        self.now = 1000.0

    def __call__(self) -> float:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += seconds


@pytest.fixture
def registry(tmp_path) -> OperatorRegistry:
    return OperatorRegistry(tmp_path / "operators.json", cost=FAST)


def make_session(registry: OperatorRegistry | None, clock: Clock | None = None, **kwargs) -> OperatorSession:
    kwargs.setdefault("idle_timeout_s", 600.0)
    return OperatorSession(registry, clock=clock or Clock(), station=STATION, **kwargs)


# ---------------------------------------------------------------------------
# Senhas
# ---------------------------------------------------------------------------


def test_password_hash_roundtrip_and_never_stores_the_password():
    credential = hash_password("senha-correta-1", FAST)
    assert verify_password("senha-correta-1", credential)
    assert not verify_password("senha-correta-2", credential)
    assert "senha-correta-1" not in json.dumps(credential)
    assert hash_password("senha-correta-1", FAST)["hash"] != credential["hash"]  # sal próprio


def test_password_is_unicode_normalized():
    composed = unicodedata.normalize("NFC", "açúcar-doce")
    decomposed = unicodedata.normalize("NFD", "açúcar-doce")
    assert composed != decomposed
    assert verify_password(decomposed, hash_password(composed, FAST))


@pytest.mark.parametrize(
    "credential",
    [{}, {"kdf": "md5"}, {"kdf": "scrypt", "n": 16, "r": 1, "p": 1, "salt": "!!", "hash": "!!"},
     {"kdf": "scrypt", "n": 3, "r": 1, "p": 1, "salt": "AAAAAAAAAAAAAAAAAAAAAA==", "hash": "AAAAAAAAAAAAAAAAAAAAAA=="},
     {"kdf": "scrypt", "n": 2**22, "r": 8, "p": 1, "salt": "AAAAAAAAAAAAAAAAAAAAAA==", "hash": "AAAAAAAAAAAAAAAAAAAAAA=="}],
)
def test_malformed_credentials_never_verify(credential):
    assert not verify_password("qualquer-coisa", credential)


# ---------------------------------------------------------------------------
# Cadastro
# ---------------------------------------------------------------------------


def test_registry_persists_with_private_permissions(registry):
    registry.add("ana", "senha-da-ana-1", display="Ana Souza")
    registry.add("visita", "senha-visita-1", role=ROLE_OBSERVER)
    if os.name == "posix":
        assert stat.S_IMODE(registry.path.stat().st_mode) == 0o600
    assert "senha-da-ana-1" not in registry.path.read_text()
    reopened = OperatorRegistry(registry.path, cost=FAST)
    assert [record.name for record in reopened.records()] == ["ana", "visita"]
    assert reopened.get("ANA").display == "Ana Souza"
    assert reopened.get("visita").role == ROLE_OBSERVER
    assert reopened.verify("ana", "senha-da-ana-1") is not None
    assert reopened.verify("ana", "outra-senha-qualquer") is None


@pytest.mark.parametrize("name", ["", "a", "Ana Souza", "1ana", "ana/../x", "sys:ana", "x" * 40])
def test_registry_rejects_invalid_names(registry, name):
    with pytest.raises(OperatorError):
        registry.add(name, "senha-valida-1")


def test_registry_rejects_weak_passwords_and_duplicates(registry):
    for bad in ("curta", "ana", "x" * 300):
        with pytest.raises(OperatorError):
            registry.add("ana", bad)
    registry.add("ana", "senha-valida-1")
    with pytest.raises(OperatorError):
        registry.add("ana", "outra-senha-valida")
    with pytest.raises(OperatorError):
        registry.add("bia", "senha-valida-1", role="admin")


def test_registry_password_change_disable_role_and_remove(registry):
    registry.add("ana", "senha-valida-1")
    serial = registry.get("ana").serial
    registry.set_password("ana", "senha-nova-valida")
    assert registry.get("ana").serial == serial + 1
    assert registry.verify("ana", "senha-valida-1") is None
    assert registry.verify("ana", "senha-nova-valida") is not None
    registry.set_enabled("ana", False)
    assert registry.verify("ana", "senha-nova-valida") is None and registry.active_count() == 0
    registry.set_enabled("ana", True)
    registry.set_role("ana", ROLE_OBSERVER)
    assert registry.get("ana").role == ROLE_OBSERVER
    registry.remove("ana")
    assert registry.records() == []
    with pytest.raises(OperatorError):
        registry.set_password("ana", "senha-nova-valida")


@pytest.mark.skipif(os.name != "posix", reason="permissões POSIX")
def test_registry_refuses_group_or_world_readable_file(registry):
    registry.add("ana", "senha-valida-1")
    registry.path.chmod(0o644)
    broken = OperatorRegistry(registry.path, cost=FAST)
    assert "0600" in broken.error and broken.records() == []
    with pytest.raises(OperatorError):
        broken.add("bia", "senha-valida-1")  # não sobrescreve um cadastro que não conseguiu ler


@pytest.mark.parametrize("content", ["{", "[]", '{"version": 2, "operators": []}',
                                      '{"version": 1, "operators": [{"name": "ana"}]}',
                                      '{"version": 1, "operators": [{"name": "Ana Souza", "kdf": "scrypt", "salt": "", "hash": ""}]}'])
def test_registry_reports_unreadable_content(tmp_path, content):
    path = tmp_path / "operators.json"
    path.write_text(content)
    path.chmod(0o600)
    assert OperatorRegistry(path).error


def test_registry_picks_up_changes_made_by_another_process(registry):
    registry.add("ana", "senha-valida-1")
    other = OperatorRegistry(registry.path, cost=FAST)
    other.add("bia", "senha-valida-2")
    assert registry.get("bia") is None
    assert registry.refresh() and registry.get("bia") is not None


# ---------------------------------------------------------------------------
# Login, bloqueio por tentativas, expiração
# ---------------------------------------------------------------------------


def test_login_success_and_wrong_credentials_look_the_same(registry):
    registry.add("ana", "senha-valida-1", display="Ana Souza")
    session = make_session(registry)
    wrong_password = session.login("ana", "senha-errada-1")
    unknown_user = session.login("ninguem", "senha-valida-1")
    assert not wrong_password.ok and wrong_password.reason == unknown_user.reason == "Nome ou senha incorretos."
    result = session.login(" ANA ", "senha-valida-1")
    assert result.ok and result.actor.id == "ana" and result.actor.verified
    assert session.current() == result.actor and session.label() == "Ana Souza"


def test_disabled_operator_cannot_log_in(registry):
    registry.add("ana", "senha-valida-1")
    registry.add("bia", "senha-valida-2")
    registry.set_enabled("ana", False)
    assert not make_session(registry).login("ana", "senha-valida-1").ok


def test_repeated_failures_lock_logins_with_growing_delay(registry):
    registry.add("ana", "senha-valida-1")
    clock = Clock()
    session = make_session(registry, clock, max_failures=3, lockout_s=30.0)
    for _ in range(3):
        result = session.login("ana", "errada-errada")
    assert result.retry_after_s == 30.0
    blocked = session.login("ana", "senha-valida-1")  # a senha certa também espera
    assert not blocked.ok and blocked.retry_after_s > 0
    clock.advance(31)
    for _ in range(3):
        result = session.login("ana", "errada-errada")
    assert result.retry_after_s == 60.0  # dobra a cada bloqueio seguido
    clock.advance(61)
    assert session.login("ana", "senha-valida-1").ok


def test_session_expires_after_idle_time_and_touch_extends_it(registry):
    registry.add("ana", "senha-valida-1")
    clock = Clock()
    session = make_session(registry, clock, idle_timeout_s=600.0)
    session.login("ana", "senha-valida-1")
    clock.advance(500)
    session.touch()
    clock.advance(500)
    assert session.current() is not None and session.poll() is None
    clock.advance(101)
    assert session.current() is None
    ended = session.poll()
    assert ended is not None and ended[0].id == "ana" and ended[1] == "idle"
    assert session.poll() is None  # avisa uma vez só
    assert session.label() == "sem login"


def test_a_key_press_cannot_revive_a_session_that_already_expired(registry):
    registry.add("ana", "senha-valida-1")
    clock = Clock()
    session = make_session(registry, clock, idle_timeout_s=600.0)
    session.login("ana", "senha-valida-1")
    clock.advance(601)
    session.touch()  # antes de o relógio de verificação encerrar a sessão
    assert session.current() is None
    assert session.poll()[1] == "idle"
    assert session.login("ana", "senha-valida-1").ok  # só um novo login renova


def test_session_never_expires_when_idle_timeout_is_zero(registry):
    registry.add("ana", "senha-valida-1")
    clock = Clock()
    session = make_session(registry, clock, idle_timeout_s=0.0)
    session.login("ana", "senha-valida-1")
    clock.advance(10**6)
    assert session.current() is not None and session.seconds_left() is None


@pytest.mark.parametrize("change", ["password", "disable", "remove", "role"])
def test_session_ends_when_registry_changes_the_operator(registry, change):
    registry.add("ana", "senha-valida-1")
    registry.add("bia", "senha-valida-2")
    session = make_session(registry)
    session.login("ana", "senha-valida-1")
    admin = OperatorRegistry(registry.path, cost=FAST)  # a CLI, em outro processo
    {"password": lambda: admin.set_password("ana", "senha-nova-valida"),
     "disable": lambda: admin.set_enabled("ana", False),
     "remove": lambda: admin.remove("ana"),
     "role": lambda: admin.set_role("ana", ROLE_OBSERVER)}[change]()
    assert session.current() is None
    ended = session.poll()
    assert ended is not None and ended[1] == "revoked"


def test_logout_returns_who_left(registry):
    registry.add("ana", "senha-valida-1")
    session = make_session(registry)
    session.login("ana", "senha-valida-1")
    assert session.logout().id == "ana"
    assert session.logout() is None and session.current() is None


# ---------------------------------------------------------------------------
# Modos e decisões
# ---------------------------------------------------------------------------


def test_auto_without_registry_uses_the_system_user(registry):
    session = make_session(registry)
    assert session.kind == "station"
    decision = session.authorize(mutating=True)
    assert decision.allowed and decision.actor.id == "sys:marcelo" and not decision.actor.verified
    assert session.label() == "marcelo (sistema)"


def test_auto_with_registry_requires_login_for_mutating_commands(registry):
    registry.add("ana", "senha-valida-1")
    session = make_session(registry)
    assert session.kind == "login"
    assert session.authorize(mutating=False).allowed  # consultar é livre
    denied = session.authorize(mutating=True)
    assert not denied.allowed and denied.code == "login_required"
    session.login("ana", "senha-valida-1")
    allowed = session.authorize(mutating=True)
    assert allowed.allowed and allowed.actor.id == "ana"


def test_required_without_operators_blocks_everything_that_mutates(registry):
    session = make_session(registry, mode="required")
    denied = session.authorize(mutating=True)
    assert not denied.allowed and denied.code == "no_operators"
    assert session.authorize(mutating=False).allowed
    assert not session.login("ana", "qualquer-senha").ok


def test_observer_can_look_but_not_change(registry):
    registry.add("visita", "senha-valida-1", role=ROLE_OBSERVER)
    session = make_session(registry)
    session.login("visita", "senha-valida-1")
    assert session.authorize(mutating=False).allowed
    denied = session.authorize(mutating=True)
    assert not denied.allowed and denied.code == "read_only" and denied.actor.id == "visita"
    assert "(observador)" in session.label()


@pytest.mark.skipif(os.name != "posix", reason="permissões POSIX")
def test_broken_registry_fails_closed_instead_of_falling_back_to_the_system_user(registry):
    registry.add("ana", "senha-valida-1")
    registry.path.chmod(0o666)
    session = make_session(OperatorRegistry(registry.path, cost=FAST))
    assert session.kind == "login"  # apagar ou estragar o arquivo não pode desligar o login
    denied = session.authorize(mutating=True)
    assert not denied.allowed and denied.code == "registry_error"


def test_off_mode_identifies_no_one_and_never_opens_the_registry(tmp_path, monkeypatch):
    monkeypatch.setenv("IOT_OVER_CAN_CONFIG_DIR", str(tmp_path))
    (tmp_path / "operators.json").write_text("isto não é json")
    session = make_session(None, mode="off")
    assert session.registry is None and session.kind == "off"
    decision = session.authorize(mutating=True)
    assert decision.allowed and decision.actor is None
    assert session.label() == "" and not session.login("ana", "x").ok


def test_actor_ids_never_collide_between_registered_and_system_users():
    registered = Actor("ana", method="password")
    from_system = Actor("ana", method="station")
    assert registered.id == "ana" and from_system.id == "sys:ana"
    assert registered.verified and not from_system.verified
    assert registered.record()["verified"] is True
    assert Actor("v", role=ROLE_OPERATOR).can_modify and not Actor("v", role=ROLE_OBSERVER).can_modify
